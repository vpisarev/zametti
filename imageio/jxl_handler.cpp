#include "jxl_handler.h"

#include "color.h"

#include <jxl/cms.h>
#include <jxl/decode.h>
#include <jxl/decode_cxx.h>
#include <jxl/resizable_parallel_runner.h>
#include <jxl/resizable_parallel_runner_cxx.h>

#include <QColorSpace>
#include <QImage>
#include <QIODevice>
#include <QVariant>
#include <QtGlobal>

#include <algorithm>
#include <thread>

namespace zametti {

namespace {

// Подпись JXL бывает двух видов: голый поток и контейнер ISOBMFF. Спрашивать
// об этом саму libjxl (JxlSignatureCheck) правильнее, чем сверять байты
// руками, — она же потом и читает.
constexpr int kSignatureBytes = 64;

// Сколько потоков отдать декодеру. Одна картинка на весь экран разжимается
// заметно, а показ ждать не любит.
uint32_t workerCount() {
    const unsigned hw = std::thread::hardware_concurrency();
    return hw == 0 ? 1u : std::min(hw, 8u);
}

}  // namespace

JxlHandler::JxlHandler() = default;
JxlHandler::~JxlHandler() = default;

bool JxlHandler::peek(QIODevice* device) {
    if (!device) return false;
    const QByteArray head = device->peek(kSignatureBytes);
    if (head.isEmpty()) return false;
    const JxlSignature sig =
        JxlSignatureCheck(reinterpret_cast<const uint8_t*>(head.constData()), size_t(head.size()));
    // NOT_ENOUGH_BYTES означает «пока не знаю»: у усечённого потока подпись
    // может быть впереди. Для canRead этого мало — говорим «нет», иначе
    // перехватим чужой формат.
    return sig == JXL_SIG_CODESTREAM || sig == JXL_SIG_CONTAINER;
}

bool JxlHandler::canRead() const {
    if (headerFailed_) return false;
    if (scanned_) return false;
    return peek(device());
}

bool JxlHandler::readHeader() const {
    if (haveHeader_) return true;
    if (headerFailed_) return false;
    QIODevice* dev = device();
    if (!dev) { headerFailed_ = true; return false; }

    if (data_.isEmpty()) data_ = dev->readAll();
    if (data_.isEmpty()) { headerFailed_ = true; return false; }

    auto dec = JxlDecoderMake(nullptr);
    if (!dec) { headerFailed_ = true; return false; }
    if (JxlDecoderSubscribeEvents(dec.get(), JXL_DEC_BASIC_INFO | JXL_DEC_COLOR_ENCODING) !=
        JXL_DEC_SUCCESS) {
        headerFailed_ = true;
        return false;
    }
    JxlDecoderSetInput(dec.get(), reinterpret_cast<const uint8_t*>(data_.constData()),
                       size_t(data_.size()));
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
            size_t need = 0;
            if (JxlDecoderGetICCProfileSize(dec.get(), JXL_COLOR_PROFILE_TARGET_ORIGINAL,
                                            &need) == JXL_DEC_SUCCESS &&
                need > 0) {
                QByteArray icc(qsizetype(need), Qt::Uninitialized);
                if (JxlDecoderGetColorAsICCProfile(dec.get(), JXL_COLOR_PROFILE_TARGET_ORIGINAL,
                                                   reinterpret_cast<uint8_t*>(icc.data()),
                                                   need) == JXL_DEC_SUCCESS) {
                    icc_ = icc;
                }
            }
            // И ОТДЕЛЬНО — пространство, в котором декодер отдаёт пиксели. У
            // lossy-файла оно СВОЁ, линейное, и с исходным не совпадает.
            size_t dataNeed = 0;
            if (JxlDecoderGetICCProfileSize(dec.get(), JXL_COLOR_PROFILE_TARGET_DATA,
                                            &dataNeed) == JXL_DEC_SUCCESS &&
                dataNeed > 0) {
                QByteArray icc(qsizetype(dataNeed), Qt::Uninitialized);
                if (JxlDecoderGetColorAsICCProfile(dec.get(), JXL_COLOR_PROFILE_TARGET_DATA,
                                                   reinterpret_cast<uint8_t*>(icc.data()),
                                                   dataNeed) == JXL_DEC_SUCCESS) {
                    iccData_ = icc;
                }
            }
            break;   // всё, что нужно из шапки, собрано
        } else {
            break;   // ошибка, конец входа или неожиданное событие
        }
    }
    if (!gotInfo) { headerFailed_ = true; return false; }

    size_ = QSize(int(info.xsize), int(info.ysize));
    bitsPerSample_ = int(info.bits_per_sample);
    hasAlpha_ = info.alpha_bits > 0;

    // Ориентация JXL пронумерована как в EXIF, и Qt-шные преобразования тоже.
    // Перечисляем руками: молчаливое приведение чисел друг к другу — способ
    // однажды получить зеркальное отражение вместо поворота.
    switch (info.orientation) {
        case JXL_ORIENT_IDENTITY:        transform_ = TransformationNone; break;
        case JXL_ORIENT_FLIP_HORIZONTAL: transform_ = TransformationMirror; break;
        case JXL_ORIENT_ROTATE_180:      transform_ = TransformationRotate180; break;
        case JXL_ORIENT_FLIP_VERTICAL:   transform_ = TransformationFlip; break;
        case JXL_ORIENT_TRANSPOSE:       transform_ = TransformationFlipAndRotate90; break;
        case JXL_ORIENT_ROTATE_90_CW:    transform_ = TransformationRotate90; break;
        case JXL_ORIENT_ANTI_TRANSPOSE:  transform_ = TransformationMirrorAndRotate90; break;
        case JXL_ORIENT_ROTATE_90_CCW:   transform_ = TransformationRotate270; break;
        default:                         transform_ = TransformationNone; break;
    }

    haveHeader_ = true;
    return true;
}

bool JxlHandler::read(QImage* image) {
    if (!image) return false;
    if (scanned_) return false;
    if (!readHeader()) return false;
    if (size_.isEmpty()) return false;

    // Глубже восьми бит — читаем в шестнадцатибитный формат Qt. Ужать 10 или
    // 12 бит до восьми значило бы потерять то, ради чего JXL и выбран.
    const bool deep = bitsPerSample_ > 8;
    const QImage::Format format =
        deep ? (hasAlpha_ ? QImage::Format_RGBA64 : QImage::Format_RGBX64)
             : (hasAlpha_ ? QImage::Format_RGBA8888 : QImage::Format_RGBX8888);

    QImage out(size_, format);
    if (out.isNull()) return false;   // потолок аллокации Qt сказал «нет»

    auto dec = JxlDecoderMake(nullptr);
    auto runner = JxlResizableParallelRunnerMake(nullptr);
    if (!dec || !runner) return false;
    JxlResizableParallelRunnerSetThreads(runner.get(), workerCount());
    if (JxlDecoderSetParallelRunner(dec.get(), JxlResizableParallelRunner, runner.get()) !=
        JXL_DEC_SUCCESS)
        return false;
    if (JxlDecoderSubscribeEvents(dec.get(), JXL_DEC_COLOR_ENCODING | JXL_DEC_FULL_IMAGE) !=
        JXL_DEC_SUCCESS)
        return false;
    // Модуль управления цветом нужен, чтобы попросить вывод В ИСХОДНОМ
    // пространстве: без него декодер отдаёт своё внутреннее.
    JxlDecoderSetCms(dec.get(), *JxlGetDefaultCms());
    JxlDecoderSetInput(dec.get(), reinterpret_cast<const uint8_t*>(data_.constData()),
                       size_t(data_.size()));
    JxlDecoderCloseInput(dec.get());

    // Четыре канала всегда: у Qt нет трёхбайтового формата с тем же порядком,
    // а RGBX/RGBA64 выровнены и годятся под прямую запись построчно.
    const JxlPixelFormat fmt{4, deep ? JXL_TYPE_UINT16 : JXL_TYPE_UINT8, JXL_NATIVE_ENDIAN, 0};

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
            const size_t rowBytes = size_t(size_.width()) * 4u * (deep ? 2u : 1u);
            if (size_t(out.bytesPerLine()) == rowBytes) {
                if (JxlDecoderSetImageOutBuffer(dec.get(), &fmt, out.bits(),
                                                size_t(out.sizeInBytes())) != JXL_DEC_SUCCESS)
                    return false;
            } else {
                data_.squeeze();
                QByteArray tmp(qsizetype(rowBytes * size_t(size_.height())), Qt::Uninitialized);
                if (JxlDecoderSetImageOutBuffer(dec.get(), &fmt, tmp.data(),
                                                size_t(tmp.size())) != JXL_DEC_SUCCESS)
                    return false;
                // Строки перенесём после декода — держим буфер живым.
                scratch_ = tmp;
            }
        } else if (st == JXL_DEC_FULL_IMAGE) {
            done = true;
            break;
        } else if (st == JXL_DEC_SUCCESS) {
            break;
        } else {
            return false;   // ошибка или оборванный вход
        }
    }
    if (!done) return false;

    if (!scratch_.isEmpty()) {
        const size_t rowBytes = size_t(size_.width()) * 4u * (deep ? 2u : 1u);
        for (int y = 0; y < size_.height(); ++y)
            std::memcpy(out.scanLine(y), scratch_.constData() + size_t(y) * rowBytes, rowBytes);
        scratch_.clear();
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
    // средствами Qt. Но Qt не разбирает 30-килобайтный профиль Apple из бокса
    // JXL, хотя из JPEG разбирает, — и перевод молча не происходит.
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
    //
    // Как ошибка нашлась: SSIMULACRA2 давала около минус сорока ОДИНАКОВО при
    // любом качестве. Постоянство и выдало причину — расхождение не от сжатия,
    // а от цвета. Файлы в sRGB не задеты: у них оба профиля совпадают.
    const QColorSpace dataSpace =
        iccData_.isEmpty() ? QColorSpace() : QColorSpace::fromIccProfile(iccData_);
    const QColorSpace originalSpace =
        icc_.isEmpty() ? QColorSpace() : QColorSpace::fromIccProfile(icc_);

    if (!icc_.isEmpty() && !iccData_.isEmpty() && icc_ != iccData_ &&
        convertIcc(out, iccData_, icc_)) {
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

    *image = out;
    scanned_ = true;
    return true;
}

QVariant JxlHandler::option(ImageOption option) const {
    switch (option) {
        case Size:
            return readHeader() ? QVariant(size_) : QVariant();
        case ImageFormat:
            if (!readHeader()) return QVariant();
            return QVariant::fromValue(bitsPerSample_ > 8
                                           ? (hasAlpha_ ? QImage::Format_RGBA64
                                                        : QImage::Format_RGBX64)
                                           : (hasAlpha_ ? QImage::Format_RGBA8888
                                                        : QImage::Format_RGBX8888));
        case ImageTransformation:
            return readHeader() ? QVariant::fromValue(int(transform_)) : QVariant();
        default:
            return QVariant();
    }
}

void JxlHandler::setOption(ImageOption, const QVariant&) {
    // Читающий обработчик: настраивать нечего. Поворот мы только СООБЩАЕМ —
    // применяет его Qt, и это правильно: так же он поступает с JPEG.
}

bool JxlHandler::supportsOption(ImageOption option) const {
    return option == Size || option == ImageFormat || option == ImageTransformation;
}

}  // namespace zametti
