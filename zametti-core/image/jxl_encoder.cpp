#include "jxl_encoder.h"

#include <jxl/cms.h>
#include <jxl/encode.h>
#include <jxl/encode_cxx.h>
#include <jxl/decode.h>
#include <jxl/decode_cxx.h>
#include <jxl/thread_parallel_runner.h>
#include <jxl/thread_parallel_runner_cxx.h>

#include <QColorSpace>

#include <algorithm>
#include <cstring>
#include <thread>

namespace zametti {

namespace {

// Что именно не понравилось libjxl. Своя догадка вместо её ответа — способ
// чинить не то: наступал.
const char* encoderError(JxlEncoder* enc) {
    switch (JxlEncoderGetError(enc)) {
        case JXL_ENC_ERR_OK: return "no error";
        case JXL_ENC_ERR_GENERIC: return "generic error";
        case JXL_ENC_ERR_OOM: return "out of memory";
        case JXL_ENC_ERR_JBRD: return "cannot write JPEG reconstruction data";
        case JXL_ENC_ERR_BAD_INPUT: return "input rejected";
        case JXL_ENC_ERR_NOT_SUPPORTED: return "settings combination not supported";
        case JXL_ENC_ERR_API_USAGE: return "API call order violated";
        default: return "unknown error";
    }
}

uint32_t workerCount(int requested) {
    if (requested > 0) return uint32_t(requested);
    const unsigned hw = std::thread::hardware_concurrency();
    return hw == 0 ? 1u : hw;
}

// Вычерпывает выход энкодера. Буфер растёт удвоением: заранее размер выхода
// неизвестен, а спрашивать libjxl о нём нельзя — она сама его не знает.
QByteArray drain(JxlEncoder* enc, QString* error) {
    QByteArray out(1 << 16, Qt::Uninitialized);
    uint8_t* next = reinterpret_cast<uint8_t*>(out.data());
    size_t avail = size_t(out.size());
    JxlEncoderStatus st = JXL_ENC_NEED_MORE_OUTPUT;
    while (st == JXL_ENC_NEED_MORE_OUTPUT) {
        st = JxlEncoderProcessOutput(enc, &next, &avail);
        if (st == JXL_ENC_NEED_MORE_OUTPUT) {
            const qsizetype used = out.size() - qsizetype(avail);
            out.resize(out.size() * 2);
            next = reinterpret_cast<uint8_t*>(out.data()) + used;
            avail = size_t(out.size() - used);
        }
    }
    if (st != JXL_ENC_SUCCESS) {
        if (error)
            *error = QStringLiteral("JXL encoder refused to write: %1")
                         .arg(QString::fromLatin1(encoderError(enc)));
        return {};
    }
    out.resize(out.size() - qsizetype(avail));
    return out;
}

// Боксы контейнера. У EXIF перед заголовком TIFF идут ЧЕТЫРЕ НУЛЕВЫХ БАЙТА —
// это смещение внутри блока, и без них блоб формально записан, но читается
// неправильно. Подсмотрено у самой libjxl (lib/jxl/encode.cc): она делает так
// же, когда переносит EXIF из JPEG.
bool addBoxes(JxlEncoder* enc, const EncodeMeta& meta, QString* error) {
    if (meta.exif.isEmpty() && meta.xmp.isEmpty()) return true;
    if (JxlEncoderUseBoxes(enc) != JXL_ENC_SUCCESS) {
        if (error) *error = QStringLiteral("JXL container rejected the boxes");
        return false;
    }
    if (!meta.exif.isEmpty()) {
        QByteArray box(4, '\0');
        box += meta.exif;
        if (JxlEncoderAddBox(enc, "Exif", reinterpret_cast<const uint8_t*>(box.constData()),
                             size_t(box.size()), JXL_FALSE) != JXL_ENC_SUCCESS) {
            if (error) *error = QStringLiteral("EXIF box not written");
            return false;
        }
    }
    if (!meta.xmp.isEmpty()) {
        if (JxlEncoderAddBox(enc, "xml ",
                             reinterpret_cast<const uint8_t*>(meta.xmp.constData()),
                             size_t(meta.xmp.size()), JXL_FALSE) != JXL_ENC_SUCCESS) {
            if (error) *error = QStringLiteral("XMP box not written");
            return false;
        }
    }
    return true;
}

}  // namespace

QByteArray encodeJxl(const QImage& image, const EncodeOptions& options, const EncodeMeta& meta,
                     QString* error) {
    auto fail = [&](const QString& why) {
        if (error) *error = why;
        return QByteArray();
    };
    if (image.isNull()) return fail(QStringLiteral("nothing to encode"));

    // Глубина берётся из формата картинки. Шире потолка не пишем: 16-битный
    // источник при потолке 12 объявляется двенадцатибитным, и ужимает libjxl.
    const bool deep = image.depth() > 32;
    const int srcBits = deep ? 16 : 8;
    const int bits = std::min(srcBits, std::max(1, options.maxBitsPerChannel));
    const bool alpha = image.hasAlphaChannel();

    // ЧИСЛО КАНАЛОВ ОБЯЗАНО СОВПАСТЬ с объявленным в описании картинки. Отдать
    // четыре канала там, где объявлено три, — не мелочь: libjxl отвечает
    // «нарушен порядок вызовов», и без её собственного текста ошибки причину
    // было не найти (наступал).
    //
    // У Qt нет трёхбайтового ГЛУБОКОГО формата, поэтому при шестнадцати битах
    // без альфы приходится перекладывать строки самим, выбрасывая ненужный
    // четвёртый канал.
    const int channels = alpha ? 4 : 3;
    const QImage::Format want = deep ? (alpha ? QImage::Format_RGBA64 : QImage::Format_RGBX64)
                                     : (alpha ? QImage::Format_RGBA8888
                                              : QImage::Format_RGB888);
    const QImage src = image.format() == want ? image : image.convertToFormat(want);
    if (src.isNull()) return fail(QStringLiteral("cannot convert the image to the required form"));

    auto enc = JxlEncoderMake(nullptr);
    if (!enc) return fail(QStringLiteral("JXL encoder creation failed"));

    auto runner = JxlThreadParallelRunnerMake(nullptr, workerCount(options.threads));
    if (runner)
        JxlEncoderSetParallelRunner(enc.get(), JxlThreadParallelRunner, runner.get());

    // МОДУЛЬ УПРАВЛЕНИЯ ЦВЕТОМ. Без него libjxl не умеет перевести пиксели из
    // произвольного ICC-профиля в своё внутреннее пространство XYB — и молча
    // выдаёт систематически неверные цвета.
    //
    // Наступал: снимки с телефона в Apple Wide Color (треть каталога владельца)
    // давали SSIMULACRA2 около минус сорока, ОДИНАКОВО при любом качестве.
    // Постоянство оценки и выдало причину: расхождение не от сжатия, а от
    // цвета. Файлы в sRGB при этом были в порядке — там переводить нечего.
    JxlEncoderSetCms(enc.get(), *JxlGetDefaultCms());

    JxlBasicInfo info;
    JxlEncoderInitBasicInfo(&info);
    info.xsize = uint32_t(src.width());
    info.ysize = uint32_t(src.height());
    info.bits_per_sample = uint32_t(bits);
    info.num_color_channels = 3;
    info.num_extra_channels = alpha ? 1 : 0;
    info.alpha_bits = alpha ? uint32_t(bits) : 0;
    // Для lossless просим не переводить во внутреннее XYB: иначе «без потерь»
    // потеряет. Ровно та ловушка, ради которой флаг и существует.
    info.uses_original_profile = options.lossless ? JXL_TRUE : JXL_FALSE;
    if (JxlEncoderSetBasicInfo(enc.get(), &info) != JXL_ENC_SUCCESS)
        return fail(QStringLiteral("encoder rejected the image description"));

    // ЦВЕТ. Профиль источника переносим как есть; своего не выдумываем и в
    // sRGB не переводим — это была бы потеря на ровном месте.
    const QByteArray icc = !meta.icc.isEmpty() ? meta.icc : src.colorSpace().iccProfile();
    bool colorSet = false;
    if (!icc.isEmpty()) {
        colorSet = JxlEncoderSetICCProfile(enc.get(),
                                           reinterpret_cast<const uint8_t*>(icc.constData()),
                                           size_t(icc.size())) == JXL_ENC_SUCCESS;
    }
    if (!colorSet) {
        // Профиля нет вовсе — только тогда объявляем sRGB. Это не перевод, а
        // признание: других сведений о цвете у нас нет.
        JxlColorEncoding color = {};
        JxlColorEncodingSetToSRGB(&color, JXL_FALSE);
        if (JxlEncoderSetColorEncoding(enc.get(), &color) != JXL_ENC_SUCCESS)
            return fail(QStringLiteral("encoder rejected the color space"));
    }

    if (!addBoxes(enc.get(), meta, error)) return {};

    JxlEncoderFrameSettings* fs = JxlEncoderFrameSettingsCreate(enc.get(), nullptr);
    JxlEncoderFrameSettingsSetOption(fs, JXL_ENC_FRAME_SETTING_EFFORT,
                                     std::clamp(options.effort, 1, 9));
    if (options.lossless) {
        JxlEncoderSetFrameLossless(fs, JXL_TRUE);
        JxlEncoderSetFrameDistance(fs, 0.0f);
    } else {
        JxlEncoderSetFrameDistance(
            fs, JxlEncoderDistanceFromQuality(float(std::clamp(options.quality, 1, 100))));
    }

    const JxlPixelFormat fmt{uint32_t(channels), deep ? JXL_TYPE_UINT16 : JXL_TYPE_UINT8,
                             JXL_NATIVE_ENDIAN, 0};
    const size_t sampleBytes = deep ? 2u : 1u;
    const size_t rowBytes = size_t(src.width()) * size_t(channels) * sampleBytes;

    QByteArray packed;
    const uint8_t* pixels = nullptr;
    size_t pixelBytes = 0;
    const bool deepNoAlpha = deep && !alpha;   // единственный случай, где надо выбрасывать канал
    // Строки Qt бывают шире полезных данных — тогда отдавать её память нельзя,
    // иначе энкодер прочтёт выравнивающий мусор как пиксели.
    if (!deepNoAlpha && size_t(src.bytesPerLine()) == rowBytes) {
        pixels = src.constBits();
        pixelBytes = size_t(src.sizeInBytes());
    } else {
        packed.resize(qsizetype(rowBytes * size_t(src.height())));
        for (int y = 0; y < src.height(); ++y) {
            if (deepNoAlpha) {
                const auto* s16 = reinterpret_cast<const uint16_t*>(src.constScanLine(y));
                auto* d16 = reinterpret_cast<uint16_t*>(packed.data() + size_t(y) * rowBytes);
                for (int x = 0; x < src.width(); ++x)
                    for (int c = 0; c < 3; ++c) d16[size_t(x) * 3 + c] = s16[size_t(x) * 4 + c];
            } else {
                std::memcpy(packed.data() + size_t(y) * rowBytes, src.constScanLine(y), rowBytes);
            }
        }
        pixels = reinterpret_cast<const uint8_t*>(packed.constData());
        pixelBytes = size_t(packed.size());
    }

    if (JxlEncoderAddImageFrame(fs, &fmt, pixels, pixelBytes) != JXL_ENC_SUCCESS)
        return fail(QStringLiteral("encoder rejected the pixels: %1")
                        .arg(QString::fromLatin1(encoderError(enc.get()))));
    JxlEncoderCloseInput(enc.get());

    return drain(enc.get(), error);
}

QByteArray transcodeJpegToJxl(const QByteArray& jpeg, QString* error) {
    auto enc = JxlEncoderMake(nullptr);
    if (!enc) {
        if (error) *error = QStringLiteral("JXL encoder creation failed");
        return {};
    }
    // Контейнер обязателен: без него негде хранить данные реконструкции.
    JxlEncoderUseContainer(enc.get(), JXL_TRUE);
    if (JxlEncoderStoreJPEGMetadata(enc.get(), JXL_TRUE) != JXL_ENC_SUCCESS) {
        if (error) *error = QStringLiteral("libjxl keeps no reconstruction data");
        return {};
    }
    JxlEncoderFrameSettings* fs = JxlEncoderFrameSettingsCreate(enc.get(), nullptr);
    if (JxlEncoderAddJPEGFrame(fs, reinterpret_cast<const uint8_t*>(jpeg.constData()),
                               size_t(jpeg.size())) != JXL_ENC_SUCCESS) {
        // Обычное дело, а не поломка: не всякий JPEG к транскоду пригоден.
        if (error) *error = QStringLiteral("this JPEG is unsuitable for transcode");
        return {};
    }
    JxlEncoderCloseInput(enc.get());
    return drain(enc.get(), error);
}

QByteArray reconstructJpeg(const QByteArray& jxl, QString* error) {
    auto dec = JxlDecoderMake(nullptr);
    if (!dec) {
        if (error) *error = QStringLiteral("JXL decoder creation failed");
        return {};
    }
    if (JxlDecoderSubscribeEvents(dec.get(),
                                  JXL_DEC_JPEG_RECONSTRUCTION | JXL_DEC_FULL_IMAGE) !=
        JXL_DEC_SUCCESS) {
        if (error) *error = QStringLiteral("decoder rejected the event subscription");
        return {};
    }
    JxlDecoderSetInput(dec.get(), reinterpret_cast<const uint8_t*>(jxl.constData()),
                       size_t(jxl.size()));
    JxlDecoderCloseInput(dec.get());

    QByteArray out(1 << 20, Qt::Uninitialized);
    bool armed = false;
    for (;;) {
        const JxlDecoderStatus st = JxlDecoderProcessInput(dec.get());
        if (st == JXL_DEC_JPEG_RECONSTRUCTION) {
            JxlDecoderSetJPEGBuffer(dec.get(), reinterpret_cast<uint8_t*>(out.data()),
                                    size_t(out.size()));
            armed = true;
        } else if (st == JXL_DEC_JPEG_NEED_MORE_OUTPUT) {
            const size_t left = JxlDecoderReleaseJPEGBuffer(dec.get());
            const qsizetype used = out.size() - qsizetype(left);
            out.resize(out.size() * 2);
            JxlDecoderSetJPEGBuffer(dec.get(), reinterpret_cast<uint8_t*>(out.data()) + used,
                                    size_t(out.size() - used));
        } else if (st == JXL_DEC_FULL_IMAGE || st == JXL_DEC_SUCCESS) {
            if (!armed) {
                if (error) *error = QStringLiteral("this JXL has no data to rebuild the JPEG");
                return {};
            }
            const size_t left = JxlDecoderReleaseJPEGBuffer(dec.get());
            out.resize(out.size() - qsizetype(left));
            return out;
        } else {
            if (error) *error = QStringLiteral("JPEG rebuild failed");
            return {};
        }
    }
}

}  // namespace zametti
