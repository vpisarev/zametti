#include "jxl_read.h"

#include "color.h"

#include <jxl/cms.h>
#include <jxl/decode.h>
#include <jxl/decode_cxx.h>
#include <jxl/resizable_parallel_runner.h>
#include <jxl/resizable_parallel_runner_cxx.h>

#include <QColorSpace>

#include <algorithm>
#include <cstring>
#include <thread>

namespace zametti {

namespace {

constexpr int kSignatureBytes = 64;

// Сколько потоков отдать декодеру. Одна картинка на весь экран разжимается
// заметно, а показ ждать не любит.
uint32_t workerCount() {
    const unsigned hw = std::thread::hardware_concurrency();
    return hw == 0 ? 1u : std::min(hw, 8u);
}

// Ориентация JXL пронумерована как в EXIF, и наша Orientation тоже.
// Перечисляем руками: молчаливое приведение чисел друг к другу — способ
// однажды получить зеркальное отражение вместо поворота.
Orientation orientationOf(JxlOrientation value) {
    switch (value) {
        case JXL_ORIENT_IDENTITY:        return Orientation::Normal;
        case JXL_ORIENT_FLIP_HORIZONTAL: return Orientation::FlipHorizontal;
        case JXL_ORIENT_ROTATE_180:      return Orientation::Rotate180;
        case JXL_ORIENT_FLIP_VERTICAL:   return Orientation::FlipVertical;
        case JXL_ORIENT_TRANSPOSE:       return Orientation::Transpose;
        case JXL_ORIENT_ROTATE_90_CW:    return Orientation::Rotate90;
        case JXL_ORIENT_ANTI_TRANSPOSE:  return Orientation::AntiTranspose;
        case JXL_ORIENT_ROTATE_90_CCW:   return Orientation::Rotate270;
    }
    return Orientation::Normal;
}

QByteArray iccOf(JxlDecoder* dec, JxlColorProfileTarget target) {
    size_t need = 0;
    if (JxlDecoderGetICCProfileSize(dec, target, &need) != JXL_DEC_SUCCESS || need == 0) return {};
    QByteArray icc(qsizetype(need), Qt::Uninitialized);
    if (JxlDecoderGetColorAsICCProfile(dec, target, reinterpret_cast<uint8_t*>(icc.data()),
                                       need) != JXL_DEC_SUCCESS)
        return {};
    return icc;
}

}  // namespace

bool looksLikeJxl(const QByteArray& head) {
    if (head.isEmpty()) return false;
    const JxlSignature sig = JxlSignatureCheck(
        reinterpret_cast<const uint8_t*>(head.constData()),
        size_t(std::min<qsizetype>(head.size(), kSignatureBytes)));
    // NOT_ENOUGH_BYTES означает «пока не знаю»: у усечённого потока подпись
    // может быть впереди. Этого мало — говорим «нет», иначе перехватим чужой
    // формат.
    return sig == JXL_SIG_CODESTREAM || sig == JXL_SIG_CONTAINER;
}

JxlHeader readJxlHeader(const QByteArray& bytes) {
    JxlHeader out;
    if (bytes.isEmpty() || !looksLikeJxl(bytes)) return out;

    auto dec = JxlDecoderMake(nullptr);
    if (!dec) return out;
    if (JxlDecoderSubscribeEvents(dec.get(), JXL_DEC_BASIC_INFO | JXL_DEC_COLOR_ENCODING) !=
        JXL_DEC_SUCCESS)
        return out;
    JxlDecoderSetInput(dec.get(), reinterpret_cast<const uint8_t*>(bytes.constData()),
                       size_t(bytes.size()));
    JxlDecoderCloseInput(dec.get());

    JxlBasicInfo info{};
    bool gotInfo = false;
    for (;;) {
        const JxlDecoderStatus st = JxlDecoderProcessInput(dec.get());
        if (st == JXL_DEC_BASIC_INFO) {
            if (JxlDecoderGetBasicInfo(dec.get(), &info) != JXL_DEC_SUCCESS) break;
            gotInfo = true;
        } else if (st == JXL_DEC_COLOR_ENCODING) {
            // ИМЕННО ORIGINAL, а не DATA. У lossy-файла пиксели внутри лежат
            // в собственном пространстве libjxl, и DATA описывает ЕГО, а не то,
            // в чём картинка была. Взяв DATA, мы получили бы профиль от одного,
            // а значения от другого.
            //
            // Наступал: снимки в Apple Wide Color давали SSIMULACRA2 около
            // минус сорока — ОДИНАКОВО при любом качестве. Постоянство оценки и
            // выдало причину: расхождение не от сжатия, а от цвета. Файлы в
            // sRGB были в порядке, потому что там оба профиля совпадают.
            out.icc = iccOf(dec.get(), JXL_COLOR_PROFILE_TARGET_ORIGINAL);
            out.iccOfData = iccOf(dec.get(), JXL_COLOR_PROFILE_TARGET_DATA);
            break;   // всё, что нужно из шапки, собрано
        } else {
            break;   // ошибка, конец входа или неожиданное событие
        }
    }
    if (!gotInfo) return JxlHeader{};

    out.size = QSize(int(info.xsize), int(info.ysize));
    out.bitsPerSample = int(info.bits_per_sample);
    out.hasAlpha = info.alpha_bits > 0;
    out.orientation = orientationOf(JxlOrientation(info.orientation));
    return out;
}

QImage decodeJxl(const QByteArray& bytes) {
    const JxlHeader head = readJxlHeader(bytes);
    if (!head.valid()) return {};

    // Глубже восьми бит — читаем в шестнадцатибитный формат Qt. Ужать 10 или
    // 12 бит до восьми значило бы потерять то, ради чего JXL и выбран.
    const bool deep = head.bitsPerSample > 8;
    const QImage::Format format =
        deep ? (head.hasAlpha ? QImage::Format_RGBA64 : QImage::Format_RGBX64)
             : (head.hasAlpha ? QImage::Format_RGBA8888 : QImage::Format_RGBX8888);

    QImage out(head.size, format);
    if (out.isNull()) return {};   // потолок аллокации сказал «нет»

    auto dec = JxlDecoderMake(nullptr);
    auto runner = JxlResizableParallelRunnerMake(nullptr);
    if (!dec || !runner) return {};
    JxlResizableParallelRunnerSetThreads(runner.get(), workerCount());
    if (JxlDecoderSetParallelRunner(dec.get(), JxlResizableParallelRunner, runner.get()) !=
        JXL_DEC_SUCCESS)
        return {};
    if (JxlDecoderSubscribeEvents(dec.get(), JXL_DEC_COLOR_ENCODING | JXL_DEC_FULL_IMAGE) !=
        JXL_DEC_SUCCESS)
        return {};
    // Модуль управления цветом нужен, чтобы попросить вывод В ИСХОДНОМ
    // пространстве: без него декодер отдаёт своё внутреннее.
    JxlDecoderSetCms(dec.get(), *JxlGetDefaultCms());
    JxlDecoderSetInput(dec.get(), reinterpret_cast<const uint8_t*>(bytes.constData()),
                       size_t(bytes.size()));
    JxlDecoderCloseInput(dec.get());

    // Четыре канала всегда: у Qt нет трёхбайтового формата с тем же порядком,
    // а RGBX/RGBA64 выровнены и годятся под прямую запись построчно.
    const JxlPixelFormat fmt{4, deep ? JXL_TYPE_UINT16 : JXL_TYPE_UINT8, JXL_NATIVE_ENDIAN, 0};
    const size_t rowBytes = size_t(head.size.width()) * 4u * (deep ? 2u : 1u);

    QByteArray scratch;   // на случай, если шаг строки Qt шире строки картинки
    bool done = false;
    for (;;) {
        const JxlDecoderStatus st = JxlDecoderProcessInput(dec.get());
        if (st == JXL_DEC_COLOR_ENCODING) {
            // ЗДЕСЬ ОСТАЛАСЬ НЕЗАКРЫТАЯ БЕДА, см. длинный комментарий ниже, у
            // выставления профиля. Просить декодер отдавать в исходном
            // пространстве пробовал двумя способами: описанием пространства
            // (не срабатывает — у Apple Wide Color профиль описанием не
            // выражается) и ICC-блобом (принимается с кодом успеха, но декод
            // после этого не даёт картинки вовсе).
        } else if (st == JXL_DEC_NEED_IMAGE_OUT_BUFFER) {
            // Отдаём саму память QImage: лишней копии кадра не нужно.
            // bytesPerLine у Qt может быть больше строки — тогда честнее
            // собрать во временный буфер, чем врать декодеру про шаг.
            if (size_t(out.bytesPerLine()) == rowBytes) {
                if (JxlDecoderSetImageOutBuffer(dec.get(), &fmt, out.bits(),
                                                size_t(out.sizeInBytes())) != JXL_DEC_SUCCESS)
                    return {};
            } else {
                scratch = QByteArray(qsizetype(rowBytes * size_t(head.size.height())),
                                     Qt::Uninitialized);
                if (JxlDecoderSetImageOutBuffer(dec.get(), &fmt, scratch.data(),
                                                size_t(scratch.size())) != JXL_DEC_SUCCESS)
                    return {};
            }
        } else if (st == JXL_DEC_FULL_IMAGE) {
            done = true;
            break;
        } else if (st == JXL_DEC_SUCCESS) {
            break;
        } else {
            return {};   // ошибка или оборванный вход
        }
    }
    if (!done) return {};

    if (!scratch.isEmpty()) {
        for (int y = 0; y < head.size.height(); ++y)
            std::memcpy(out.scanLine(y), scratch.constData() + size_t(y) * rowBytes, rowBytes);
    }

    // ЦВЕТ. Устройство такое, и оно неочевидное.
    //
    // У lossy-файла (XYB) пиксели внутри libjxl лежат в её собственном
    // ЛИНЕЙНОМ пространстве, и отдаёт она их такими. Профиль «данных» при этом
    // честно называется RGB_D65_SRG_Rel_Lin, а исходный (скажем, Apple Wide
    // Color, 30 КБ) хранится отдельно.
    //
    // Пометить линейные пиксели исходным профилем — соврать: картинка выходит
    // заметно темнее (средние 28/23/15 вместо 52/49/42 на снимке с телефона).
    // Поэтому помечаем тем, что есть на самом деле, и пробуем перевести
    // средствами libjxl. Но Qt не разбирает 30-килобайтный профиль Apple из
    // бокса JXL, хотя из JPEG разбирает, — и перевод молча не происходит.
    //
    // Значит, пиксели надо ВЕРНУТЬ из пространства данных в исходное, а не
    // просто переклеить ярлык. Перевод делает convertIcc — то есть CMS самой
    // libjxl. Не Qt: на табличных профилях (у Apple это таблица A2B без
    // обратной B2A и без обычных первичных) Qt строит преобразование по
    // угаданным первичным и ошибается втрое — 53/50/42 он переводит в
    // 37/35/28, тогда как littleCMS в 47/44/36.
    //
    // Наши собственные файлы сюда не попадают: импорт приводит цвет к
    // выражаемому описанием пространству ДО записи (см. color.h), и тогда оба
    // профиля совпадают. Ветка нужна для ЧУЖИХ JXL, записанных как придётся.
    //
    // Что пробовал и почему не подошло:
    //   * JxlDecoderSetOutputColorProfile с ОПИСАНИЕМ пространства — не
    //     срабатывает, когда исходный профиль описанием не выражается;
    //   * то же с ICC-блобом — принимается с кодом успеха, но картинки после
    //     этого не получается вовсе;
    //   * uses_original_profile = TRUE у энкодера чинит цвет, но отключает XYB,
    //     и файл распухает ВТРОЕ. Это диагноз, а не лечение.
    const QColorSpace dataSpace =
        head.iccOfData.isEmpty() ? QColorSpace() : QColorSpace::fromIccProfile(head.iccOfData);
    const QColorSpace originalSpace =
        head.icc.isEmpty() ? QColorSpace() : QColorSpace::fromIccProfile(head.icc);

    if (!head.icc.isEmpty() && !head.iccOfData.isEmpty() && head.icc != head.iccOfData &&
        convertIcc(out, head.iccOfData, head.icc)) {
        // Перевод удался — пиксели теперь ДЕЙСТВИТЕЛЬНО в исходном
        // пространстве, им и помечаем.
        if (originalSpace.isValid()) out.setColorSpace(originalSpace);
    } else if (dataSpace.isValid()) {
        // Не удался — помечаем тем, что есть на самом деле. Соврать ярлыком
        // хуже, чем показать честные, пусть и не те цвета.
        out.setColorSpace(dataSpace);
    } else if (originalSpace.isValid()) {
        out.setColorSpace(originalSpace);
    }

    return out;
}

}  // namespace zametti
