#include "tiff_reader.h"

#include "color.h"

#include <tiffio.h>

#include <QColorSpace>
#include <QFile>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <vector>

namespace zametti {

namespace {

// Ошибки и предупреждения libtiff идут в stderr, а нам нужны в сообщении
// человеку. Заводим свои обработчики и складываем последнее сказанное сюда.
thread_local QString g_lastError;

extern "C" void tiffError(const char* module, const char* fmt, va_list ap) {
    char buf[512];
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    g_lastError = module ? QStringLiteral("%1: %2").arg(QString::fromUtf8(module),
                                                        QString::fromUtf8(buf))
                         : QString::fromUtf8(buf);
}

extern "C" void tiffWarning(const char*, const char*, va_list) {
    // Предупреждения глотаем: чужие теги, которых libtiff не знает, встречаются
    // в каждом втором файле от Photoshop и к делу не относятся.
}

// --- цвет -----------------------------------------------------------------

// Lab → линейный Display P3, белая точка источника D50 (та, что у TIFF по
// стандарту), приёмника — D65.
//
// ПОЧЕМУ P3, А НЕ sRGB. Lab покрывает весь видимый охват, и перевод в sRGB
// упирает часть цветов в границу — насыщенные краски на картинах теряются
// первыми. Display P3 шире sRGB процентов на двадцать пять и при этом
// выражается описанием, то есть переживает круг записи и чтения (см. color.h).
//
// Матрица получена сверткой трёх известных: адаптация Брэдфорда D50→D65,
// затем XYZ(D65) → линейный Display P3. Проверяется не глазами, а замером:
// белая точка Lab(100,0,0) обязана дать ровно (1,1,1), а пиксель Эрмитажа —
// совпасть с независимым разбором (см. tiff_test).
void labToLinearP3(double L, double a, double b, double out[3]) {
    const double fy = (L + 16.0) / 116.0;
    const double fx = fy + a / 500.0;
    const double fz = fy - b / 200.0;
    auto finv = [](double t) {
        const double t3 = t * t * t;
        return t3 > 0.008856 ? t3 : (t - 16.0 / 116.0) / 7.787;
    };
    const double X = finv(fx) * 0.96422;
    const double Y = finv(fy) * 1.00000;
    const double Z = finv(fz) * 0.82521;
    // XYZ(D50) → линейный Display P3 (D65), адаптация Брэдфорда свёрнута
    // внутрь. Проверка, которую матрица обязана проходить: белая точка D50
    // (0.96422, 1.0, 0.82521) даёт ровно (1, 1, 1).
    out[0] =  2.4038183 * X - 0.9897174 * Y - 0.3975865 * Z;
    out[1] = -0.8422290 * X + 1.7988454 * Y + 0.0160549 * Z;
    out[2] =  0.0481867 * X - 0.0973766 * Y + 1.2735110 * Z;
}

// Кривая у Display P3 та же, что у sRGB, — этим он и удобен.
double srgbGamma(double v) {
    v = v < 0 ? 0 : (v > 1 ? 1 : v);
    return v <= 0.0031308 ? 12.92 * v : 1.055 * std::pow(v, 1 / 2.4) - 0.055;
}

uint16_t toSample16(double v) {
    const double g = srgbGamma(v) * 65535.0;
    return uint16_t(std::lround(g < 0 ? 0 : (g > 65535.0 ? 65535.0 : g)));
}

// Сколько памяти попросит разжатая картинка у нас. Считаем в double: у TIFF
// размеры — uint32, и их произведение переполняет int64 при должном старании
// (замечание владельца). Точность тут не нужна, нужно сравнение с порогом.
double decodedBytes(uint32_t w, uint32_t h, bool deep) {
    return double(w) * double(h) * 4.0 * (deep ? 2.0 : 1.0);
}

}  // namespace

bool looksLikeTiff(const QByteArray& head) {
    if (head.size() < 4) return false;
    const auto* p = reinterpret_cast<const uint8_t*>(head.constData());
    // "II" 42 или "MM" 42; BigTIFF помечен числом 43 — его libtiff тоже умеет.
    if (p[0] == 'I' && p[1] == 'I') return (p[2] == 42 || p[2] == 43) && p[3] == 0;
    if (p[0] == 'M' && p[1] == 'M') return p[2] == 0 && (p[3] == 42 || p[3] == 43);
    return false;
}

bool readTiffHeader(const QString& path, TiffHeader* out, QString* error) {
    auto fail = [&](const QString& why) {
        if (error) *error = why;
        return false;
    };
    if (!out) return fail(QStringLiteral("nowhere to put the result"));

    TIFFSetErrorHandler(tiffError);
    TIFFSetWarningHandler(tiffWarning);
    g_lastError.clear();

    TIFF* t = TIFFOpen(path.toLocal8Bit().constData(), "r");
    if (!t)
        return fail(g_lastError.isEmpty() ? QStringLiteral("file did not open as TIFF")
                                          : g_lastError);
    struct Closer {
        TIFF* t;
        ~Closer() { TIFFClose(t); }
    } closer{t};

    uint32_t w = 0, h = 0;
    uint16_t bits = 0, samples = 0, photo = 0, extra = 0;
    uint16_t* extraTypes = nullptr;
    TIFFGetField(t, TIFFTAG_IMAGEWIDTH, &w);
    TIFFGetField(t, TIFFTAG_IMAGELENGTH, &h);
    TIFFGetFieldDefaulted(t, TIFFTAG_BITSPERSAMPLE, &bits);
    TIFFGetFieldDefaulted(t, TIFFTAG_SAMPLESPERPIXEL, &samples);
    TIFFGetFieldDefaulted(t, TIFFTAG_PHOTOMETRIC, &photo);
    TIFFGetFieldDefaulted(t, TIFFTAG_EXTRASAMPLES, &extra, &extraTypes);
    if (w == 0 || h == 0) return fail(QStringLiteral("zero size in the header"));

    out->size = QSize(int(w), int(h));
    out->bitsPerSample = int(bits);
    out->hasAlpha = extra > 0;
    // Тот же вывод, что и у полного чтения: Lab и CMYK едут шестнадцатибитным
    // кадром даже при восьмибитном файле (перевод нелинеен). Кто считает место
    // под разжатую копию, обязан знать это ДО чтения.
    out->wideFrame = bits > 8 || photo == PHOTOMETRIC_CIELAB ||
                     photo == PHOTOMETRIC_ICCLAB || photo == PHOTOMETRIC_ITULAB ||
                     photo == PHOTOMETRIC_SEPARATED;

    uint32_t len = 0;
    const void* blob = nullptr;
    if (TIFFGetField(t, TIFFTAG_ICCPROFILE, &len, &blob) == 1 && len > 0)
        out->icc = QByteArray(static_cast<const char*>(blob), qsizetype(len));
    return true;
}

bool readTiff(const QString& path, TiffImage* out, QString* error, qint64 maxDecodeBytes) {
    auto fail = [&](const QString& why) {
        if (error) *error = why;
        return false;
    };
    if (!out) return fail(QStringLiteral("nowhere to put the result"));

    TIFFSetErrorHandler(tiffError);
    TIFFSetWarningHandler(tiffWarning);
    g_lastError.clear();

    TIFF* t = TIFFOpen(path.toLocal8Bit().constData(), "r");
    if (!t)
        return fail(g_lastError.isEmpty() ? QStringLiteral("file did not open as TIFF")
                                          : g_lastError);
    struct Closer {
        TIFF* t;
        ~Closer() { TIFFClose(t); }
    } closer{t};

    uint32_t w = 0, h = 0;
    uint16_t bits = 0, samples = 0, photo = 0, planar = 0;
    TIFFGetField(t, TIFFTAG_IMAGEWIDTH, &w);
    TIFFGetField(t, TIFFTAG_IMAGELENGTH, &h);
    TIFFGetFieldDefaulted(t, TIFFTAG_BITSPERSAMPLE, &bits);
    TIFFGetFieldDefaulted(t, TIFFTAG_SAMPLESPERPIXEL, &samples);
    TIFFGetFieldDefaulted(t, TIFFTAG_PHOTOMETRIC, &photo);
    TIFFGetFieldDefaulted(t, TIFFTAG_PLANARCONFIG, &planar);

    if (w == 0 || h == 0) return fail(QStringLiteral("zero size in the header"));
    if (bits != 8 && bits != 16)
        return fail(QStringLiteral("%1-bit depth not supported — need 8 or 16")
                        .arg(bits));
    if (planar != PLANARCONFIG_CONTIG)
        return fail(QStringLiteral("channels laid out in separate planes — "
                                   "such a TIFF is not readable"));

    // ГЛУБИНА ВХОДА — bits; глубина РАБОЧЕГО КАДРА может быть больше. Lab и
    // CMYK мы переводим сами, а перевод нелинеен: восьмибитный Lab покрывает
    // охват шире, чем восьмибитный RGB, и округлять результат до восьми бит
    // значило бы терять то, ради чего затевался свой читатель. Поэтому такие
    // картинки всегда едут в шестнадцатибитном кадре, а до двенадцати их
    // ужмёт уже энкодер — один раз, а не дважды.
    const bool deep = bits > 8;
    const bool wideOut = deep || photo == PHOTOMETRIC_CIELAB ||
                         photo == PHOTOMETRIC_ICCLAB || photo == PHOTOMETRIC_ITULAB ||
                         photo == PHOTOMETRIC_SEPARATED;
    // Границы ДО чтения хоть одной строки: у TIFF всё нужное известно из
    // заголовка, и выяснять про бомбу после выделения памяти незачем.
    if (maxDecodeBytes > 0 && decodedBytes(w, h, deep) > double(maxDecodeBytes))
        return fail(QStringLiteral("image %1x%2 at %3 bits does not fit the "
                                   "decompression ceiling")
                        .arg(w)
                        .arg(h)
                        .arg(bits));

    out->bitsPerSample = bits;
    out->photometric = photo;
    out->converted = false;

    // Метаданные — до пикселей: если дальше не заладится, сказать о причине всё
    // равно будет чем.
    uint32_t len = 0;
    void* blob = nullptr;
    if (TIFFGetField(t, TIFFTAG_ICCPROFILE, &len, &blob) == 1 && len > 0)
        out->icc = QByteArray(static_cast<const char*>(blob), int(len));
    if (TIFFGetField(t, TIFFTAG_XMLPACKET, &len, &blob) == 1 && len > 0)
        out->xmp = QByteArray(static_cast<const char*>(blob), int(len));

    // Альфа: у TIFF она объявляется отдельным тегом «лишних каналов».
    uint16_t extra = 0;
    uint16_t* extraTypes = nullptr;
    TIFFGetFieldDefaulted(t, TIFFTAG_EXTRASAMPLES, &extra, &extraTypes);
    const bool hasAlpha = extra > 0 && extraTypes != nullptr &&
                          (extraTypes[0] == EXTRASAMPLE_ASSOCALPHA ||
                           extraTypes[0] == EXTRASAMPLE_UNASSALPHA);

    const QImage::Format fmt = wideOut ? (hasAlpha ? QImage::Format_RGBA64
                                                   : QImage::Format_RGBX64)
                                       : (hasAlpha ? QImage::Format_RGBA8888
                                                   : QImage::Format_RGBX8888);
    QImage img(int(w), int(h), fmt);
    if (img.isNull()) return fail(QStringLiteral("out of memory for the image"));

    // CMYK ПО ПРОФИЛЮ. Наивная формула (1-C)*(1-K) ниже — не колориметрия, и
    // это видно глазом: на «Девятом вале» Айвазовского (профиль 3M Matchprint,
    // 424 КБ) она даёт средний RGB 137/86/54 против 146/105/67 у lcms, разброс
    // 78 против 64 и насыщенность на 11% выше. Владелец увидел это раньше
    // всяких замеров: наш JXL рядом с GIMP и macOS выглядел пережаренным.
    //
    // Поэтому, если профиль в файле есть, переводим по нему тем же движком,
    // которым переводим всё остальное. Сверка с lcms на той же картинке:
    // средний модуль расхождения 0.72 из 255, наибольшее 4 (tests/cmyk_probe).
    //
    // Профиля нет — остаётся наивный путь: врать нечем, а отказываться от
    // картинки из-за отсутствия профиля хуже, чем показать её приблизительно.
    std::shared_ptr<RowIccConverter> cmyk;
    if (photo == PHOTOMETRIC_SEPARATED && !out->icc.isEmpty()) {
        cmyk = std::make_shared<RowIccConverter>(out->icc, 4, displayP3Icc(), int(w));
        if (!cmyk->valid()) cmyk.reset();
    }

    const tmsize_t rowBytes = TIFFScanlineSize(t);
    if (rowBytes <= 0) return fail(QStringLiteral("zero row length"));
    // Скобки фигурные: круглые тут читаются как объявление функции.
    std::vector<uint8_t> row(static_cast<size_t>(rowBytes), 0);

    // ВАЖНО: у LZW и Deflate нет произвольного доступа — строки читаются
    // ПОДРЯД от нуля. Прыгать к нужной нельзя, libtiff об этом честно ругается.
    for (uint32_t y = 0; y < h; ++y) {
        if (TIFFReadScanline(t, row.data(), y) != 1)
            return fail(g_lastError.isEmpty()
                            ? QStringLiteral("row %1 not read").arg(y)
                            : g_lastError);
        const uint8_t* src8 = row.data();
        const auto* src16 = reinterpret_cast<const uint16_t*>(row.data());
        auto* dst8 = img.scanLine(int(y));
        auto* dst16 = reinterpret_cast<uint16_t*>(img.scanLine(int(y)));

        // Строка целиком через CMS — по отдельному пикселю такое не считают:
        // преобразование строится один раз на всю строку.
        if (cmyk) {
            const double m = deep ? 65535.0 : 255.0;
            float* in = cmyk->input();
            for (uint32_t x = 0; x < w; ++x)
                for (int c = 0; c < 4; ++c)
                    in[size_t(x) * 4 + size_t(c)] =
                        float((deep ? double(src16[size_t(x) * samples + size_t(c)])
                                    : double(src8[size_t(x) * samples + size_t(c)])) / m);
            if (!cmyk->run()) return fail(QStringLiteral("CMYK conversion failed at row %1").arg(y));
            const float* got = cmyk->output();
            // Кадр здесь всегда широкий: у PHOTOMETRIC_SEPARATED wideOut истинно.
            for (uint32_t x = 0; x < w; ++x) {
                for (int c = 0; c < 3; ++c) {
                    const float v = std::clamp(got[size_t(x) * 3 + size_t(c)], 0.0f, 1.0f);
                    dst16[size_t(x) * 4 + size_t(c)] = uint16_t(std::lround(double(v) * 65535.0));
                }
                dst16[size_t(x) * 4 + 3] = 65535;
            }
            out->color = TiffColor::Cmyk;
            out->converted = true;
            continue;
        }

        for (uint32_t x = 0; x < w; ++x) {
            double rgb[3] = {0, 0, 0};
            double alpha = 1.0;

            switch (photo) {
                case PHOTOMETRIC_CIELAB:
                case PHOTOMETRIC_ICCLAB: {
                    // ЛОВУШКА: у CIELAB знаки a и b — дополнительный код, у
                    // ICCLAB — смещение на 128. Файлы на глаз неотличимы, и
                    // спутать их значит получить чужие цвета. Тег их различает.
                    const double maxL = deep ? 65535.0 : 255.0;
                    const double L = (deep ? double(src16[size_t(x) * samples])
                                           : double(src8[size_t(x) * samples])) *
                                     100.0 / maxL;
                    double a, b;
                    if (deep) {
                        const uint16_t ra = src16[size_t(x) * samples + 1];
                        const uint16_t rb = src16[size_t(x) * samples + 2];
                        a = photo == PHOTOMETRIC_CIELAB ? double(int16_t(ra)) / 256.0
                                                        : (double(ra) - 32768.0) / 256.0;
                        b = photo == PHOTOMETRIC_CIELAB ? double(int16_t(rb)) / 256.0
                                                        : (double(rb) - 32768.0) / 256.0;
                    } else {
                        const uint8_t ra = src8[size_t(x) * samples + 1];
                        const uint8_t rb = src8[size_t(x) * samples + 2];
                        a = photo == PHOTOMETRIC_CIELAB ? double(int8_t(ra)) : double(ra) - 128.0;
                        b = photo == PHOTOMETRIC_CIELAB ? double(int8_t(rb)) : double(rb) - 128.0;
                    }
                    labToLinearP3(L, a, b, rgb);
                    out->color = TiffColor::Lab;
                    out->converted = true;
                    break;
                }
                case PHOTOMETRIC_SEPARATED: {
                    // CMYK наивно: 1-K по каждому каналу. Это НЕ колориметрия —
                    // у файла владельца лежит ICC-профиль в 424 КБ, и по-хорошему
                    // переводить надо по нему. Пока честно так же, как делают Qt
                    // и все остальные; профиль сохраняется в out->icc.
                    const double m = deep ? 65535.0 : 255.0;
                    auto s = [&](int c) {
                        return (deep ? double(src16[size_t(x) * samples + c])
                                     : double(src8[size_t(x) * samples + c])) / m;
                    };
                    const double k = s(3);
                    for (int c = 0; c < 3; ++c) rgb[c] = (1.0 - s(c)) * (1.0 - k);
                    // Значения уже с гаммой — второй раз её накладывать нельзя.
                    out->color = TiffColor::Cmyk;
                    out->converted = true;
                    if (wideOut) {
                        for (int c = 0; c < 3; ++c)
                            dst16[size_t(x) * 4 + c] = uint16_t(std::lround(rgb[c] * 65535.0));
                        dst16[size_t(x) * 4 + 3] = 65535;
                    } else {
                        for (int c = 0; c < 3; ++c)
                            dst8[size_t(x) * 4 + c] = uint8_t(std::lround(rgb[c] * 255.0));
                        dst8[size_t(x) * 4 + 3] = 255;
                    }
                    continue;
                }
                case PHOTOMETRIC_MINISBLACK:
                case PHOTOMETRIC_MINISWHITE: {
                    const double m = deep ? 65535.0 : 255.0;
                    double v = (deep ? double(src16[size_t(x) * samples])
                                     : double(src8[size_t(x) * samples])) / m;
                    if (photo == PHOTOMETRIC_MINISWHITE) v = 1.0 - v;
                    rgb[0] = rgb[1] = rgb[2] = v;
                    if (samples > 1 && hasAlpha)
                        alpha = (deep ? double(src16[size_t(x) * samples + 1])
                                      : double(src8[size_t(x) * samples + 1])) / m;
                    out->color = TiffColor::Gray;
                    // Уже с гаммой — пишем как есть.
                    if (wideOut) {
                        for (int c = 0; c < 3; ++c)
                            dst16[size_t(x) * 4 + c] = uint16_t(std::lround(rgb[c] * 65535.0));
                        dst16[size_t(x) * 4 + 3] = uint16_t(std::lround(alpha * 65535.0));
                    } else {
                        for (int c = 0; c < 3; ++c)
                            dst8[size_t(x) * 4 + c] = uint8_t(std::lround(rgb[c] * 255.0));
                        dst8[size_t(x) * 4 + 3] = uint8_t(std::lround(alpha * 255.0));
                    }
                    continue;
                }
                case PHOTOMETRIC_RGB:
                default: {
                    // Отсчёты уже в своём пространстве и со своей гаммой —
                    // трогать их нельзя, профиль поедет вместе с картинкой.
                    out->color = TiffColor::Rgb;
                    if (deep) {
                        for (int c = 0; c < 3; ++c)
                            dst16[size_t(x) * 4 + c] = src16[size_t(x) * samples + c];
                        dst16[size_t(x) * 4 + 3] =
                            hasAlpha ? src16[size_t(x) * samples + 3] : 65535;
                    } else {
                        for (int c = 0; c < 3; ++c)
                            dst8[size_t(x) * 4 + c] = src8[size_t(x) * samples + c];
                        dst8[size_t(x) * 4 + 3] = hasAlpha ? src8[size_t(x) * samples + 3] : 255;
                    }
                    continue;
                }
            }

            // Сюда доходит только Lab: у него значения линейные, гамму
            // накладываем здесь.
            if (wideOut) {
                for (int c = 0; c < 3; ++c) dst16[size_t(x) * 4 + c] = toSample16(rgb[c]);
                dst16[size_t(x) * 4 + 3] = 65535;
            } else {
                for (int c = 0; c < 3; ++c)
                    dst8[size_t(x) * 4 + c] = uint8_t(toSample16(rgb[c]) >> 8);
                dst8[size_t(x) * 4 + 3] = 255;
            }
        }
    }

    // Профиль. Перевели сами — чужой на картинку вешать НЕЛЬЗЯ: он описывал
    // исходное пространство, которого в пикселях больше нет. Пометить
    // P3-пиксели профилем Lab или печатным CMYK значило бы соврать.
    if (out->color == TiffColor::Lab || cmyk) {
        // Оба этих пути ведут в Display P3 — им и помечаем. У CMYK условие
        // именно `cmyk`, а не `color == Cmyk`: наивная формула тоже ставит
        // Cmyk, но в P3 не переводит, и метка ей не полагается.
        img.setColorSpace(QColorSpace(QColorSpace::DisplayP3));
    } else if (out->color == TiffColor::Cmyk) {
        // Наивно переведённый CMYK остаётся БЕЗ пометки. Профиля в файле не
        // было, чужого вешать нечего, а объявить эти пиксели хоть каким-то
        // известным пространством — соврать наугад. Молчание честнее: дальше
        // их прочтут как sRGB, и это ровно то приближение, которым они и
        // получены.
    } else if (!out->icc.isEmpty()) {
        const QColorSpace cs = QColorSpace::fromIccProfile(out->icc);
        if (cs.isValid()) img.setColorSpace(cs);
    }

    out->image = img;
    return true;
}

}  // namespace zametti
