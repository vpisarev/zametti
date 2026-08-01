#include "import.h"

#include "exif.h"
#include "jxl_encoder.h"
#include "ladder.h"
#include "resample.h"
#include "tiff_reader.h"

#include <QColorSpace>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>

#include <algorithm>

namespace zametti {

const char* routeName(Route route) {
    switch (route) {
        case Route::Refused: return "отказ";
        case Route::AsIs: return "как есть";
        case Route::TranscodedJpeg: return "транскод JPEG";
        case Route::Lossless: return "lossless";
        case Route::Photo: return "путь фото";
    }
    return "?";
}

namespace {

QString refusalText(Refusal r) {
    switch (r) {
        case Refusal::None: return {};
        case Refusal::NoSize:
            return QStringLiteral("не удалось прочитать размеры — файл повреждён или это не "
                                  "картинка");
        case Refusal::Empty:
            return QStringLiteral("в заголовке нулевой размер");
        case Refusal::SideTooBig:
            return QStringLiteral("сторона больше 65535 пикселей");
        case Refusal::AreaTooBig:
            return QStringLiteral("площадь больше 512 мегапикселей");
        case Refusal::TooMuchMemory:
            return QStringLiteral("разжатая картинка не влезает в отведённую память");
    }
    return {};
}

ImportResult refuse(Refusal r, const QString& extra = {}) {
    ImportResult out;
    out.route = Route::Refused;
    out.refusal = r;
    out.message = extra.isEmpty() ? refusalText(r) : extra;
    return out;
}

// Метаданные для записи: EXIF со сброшенным поворотом (пиксели мы уже
// повернули), XMP как есть.
EncodeMeta metaFor(const QByteArray& file) {
    const ImageMeta src =
        readImageMeta(std::string_view(file.constData(), size_t(file.size())));
    EncodeMeta out;
    if (!src.exif.empty()) {
        std::string exif = src.exif;
        // Поворот СБРАСЫВАЕМ: читали мы с автоповоротом, значит в пикселях он
        // уже применён. Оставить тег значит повернуть картинку второй раз.
        resetExifOrientation(&exif);
        out.exif = QByteArray(exif.data(), qsizetype(exif.size()));
    }
    if (!src.xmp.empty()) out.xmp = QByteArray(src.xmp.data(), qsizetype(src.xmp.size()));
    return out;
}

// Проба на копии ~800 px: во сколько раз lossless тяжелее lossy. Малое
// отношение означает «плоскую» картинку — скриншот, схему, рисунок, — и ей
// место в lossless.
//
// Проба — ПРЕДСКАЗАНИЕ, и она обязана быть дешевле того, что предсказывает.
// Отсюда и восемьсот пикселей: полный энкод стоил бы столько же, сколько сам
// путь фото, и смысла в предсказании не осталось бы.
double losslessRatio(const QImage& image, const ImportLimits& limits) {
    const int side = 800;
    const double k = std::min(1.0, double(side) / std::max(image.width(), image.height()));
    const QImage small = k >= 1.0 ? image
                                  : resampleArea(image, std::max(1, int(image.width() * k)),
                                                 std::max(1, int(image.height() * k)));
    EncodeOptions lossy;
    lossy.quality = limits.quality;
    EncodeOptions lossless;
    lossless.lossless = true;

    QString err;
    const QByteArray a = encodeJxl(small, lossless, EncodeMeta{}, &err);
    const QByteArray b = encodeJxl(small, lossy, EncodeMeta{}, &err);
    if (a.isEmpty() || b.isEmpty()) return 1e9;   // не смогли предсказать — на путь фото
    return double(a.size()) / double(b.size());
}

// Путь фото: уменьшить до бюджета пикселей и отдать лестнице. Лестница сама
// решит, надо ли отступать по качеству и разрешению.
ImportResult photoPath(const QImage& image, const EncodeMeta& meta, const ImportLimits& limits) {
    const Size target = targetSize({image.width(), image.height()}, limits);
    const QImage scaled = (target.width == image.width() && target.height == image.height())
                              ? image
                              : resampleArea(image, target.width, target.height);

    const LadderResult ladder = runLadder(scaled, meta, limits);
    if (!ladder.ok())
        return refuse(Refusal::None,
                      QStringLiteral("не удалось сжать: %1").arg(ladder.error));

    ImportResult out;
    out.route = Route::Photo;
    out.bytes = ladder.bytes;
    out.extension = QStringLiteral("jxl");
    out.size = {ladder.width, ladder.height};
    out.bitsPerSample = scaled.depth() > 32 ? std::min(16, limits.maxBitsPerChannel) : 8;
    out.encodes = ladder.encodes;
    out.quality = ladder.quality;
    out.ssimulacra2 = ladder.ssimulacra2;
    if (ladder.arbiterRolledBack)
        out.message = QStringLiteral("арбитр вернул ступень назад: качество дороже бюджета");
    else if (ladder.overBudget)
        out.message = QStringLiteral("принят перелёт бюджета: ниже пола качества не опускаемся");
    else if (ladder.hitQualityFloor)
        out.message = QStringLiteral("дошло до пола качества");
    return out;
}

ImportResult losslessPath(const QImage& image, const EncodeMeta& meta,
                          const ImportLimits& limits) {
    // Уменьшать «плоские» картинки нельзя так же вольно, как фотографии: у
    // скриншота текст становится нечитаемым. Бриф разрешает им быть вдвое
    // крупнее потолка стороны — до 2 × 3S.
    const int cap = 6 * limits.maxSize;
    QImage src = image;
    if (std::max(image.width(), image.height()) > cap) {
        const double k = double(cap) / std::max(image.width(), image.height());
        src = resampleArea(image, std::max(1, int(image.width() * k)),
                           std::max(1, int(image.height() * k)));
    }

    EncodeOptions opt;
    opt.lossless = true;
    opt.maxBitsPerChannel = limits.maxBitsPerChannel;

    QString err;
    const QByteArray jxl = encodeJxl(src, opt, meta, &err);
    if (jxl.isEmpty())
        return refuse(Refusal::None, QStringLiteral("не удалось сжать без потерь: %1").arg(err));

    // Правило брифа: lossless, вылезший за бюджет файла, уходит на путь фото.
    // Проба — предсказание, полный энкод — измерение, измерение главнее.
    if (jxl.size() > limits.fileBudgetBytes()) return photoPath(image, meta, limits);

    ImportResult out;
    out.route = Route::Lossless;
    out.bytes = jxl;
    out.extension = QStringLiteral("jxl");
    out.size = {src.width(), src.height()};
    out.bitsPerSample = src.depth() > 32 ? std::min(16, limits.maxBitsPerChannel) : 8;
    out.encodes = 1;
    return out;
}

}  // namespace

SourceInfo probeSource(const QString& path, const ImportLimits& limits) {
    SourceInfo info;
    const QFileInfo fi(path);
    info.fileBytes = fi.size();

    QImageReader reader(path);
    reader.setAutoTransform(true);
    info.format = QString::fromLatin1(reader.format()).toLower();
    info.animated = reader.imageCount() > 1 || reader.supportsAnimation();

    // TIFF читаем своим читателем, но РАЗМЕРЫ и здесь берём у Qt: заголовок он
    // разбирает верно, врёт он только в цвете.
    const QSize size = reader.size();
    info.size = {size.width(), size.height()};

    // Глубину Qt по заголовку не сообщает; спрашиваем формат, который он
    // собирается отдать.
    const QVariant fmt = reader.imageFormat();
    info.bitsPerSample = 8;
    if (fmt.isValid()) {
        const auto f = static_cast<QImage::Format>(fmt.toInt());
        if (f == QImage::Format_RGBA64 || f == QImage::Format_RGBX64 ||
            f == QImage::Format_Grayscale16)
            info.bitsPerSample = 16;
    }
    info.hasAlpha = fmt.isValid() &&
                    QImage(1, 1, static_cast<QImage::Format>(fmt.toInt())).hasAlphaChannel();

    info.refusal = checkSource(info.size.width, info.size.height, info.bitsPerSample, limits);
    return info;
}

ImportResult importImage(const QString& path, const ImportLimits& limits) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return refuse(Refusal::NoSize, QStringLiteral("файл не открылся"));
    const QByteArray raw = f.readAll();
    f.close();

    const SourceInfo info = probeSource(path, limits);
    if (info.refusal != Refusal::None) return refuse(info.refusal);

    ImportResult result;
    result.sourceBytes = raw.size();
    const bool fitsFile = raw.size() <= limits.fileBudgetBytes();
    const Size target = targetSize(info.size, limits);
    const bool fitsPixels = target == info.size;

    // --- JXL: уже наш формат ---------------------------------------------
    if (info.format == QLatin1String("jxl") && fitsFile && fitsPixels) {
        result.route = Route::AsIs;
        result.bytes = raw;
        result.extension = QStringLiteral("jxl");
        result.size = info.size;
        result.bitsPerSample = info.bitsPerSample;
        return result;
    }

    // --- JPEG: байт-точный транскод, если влезает в оба бюджета -----------
    if ((info.format == QLatin1String("jpeg") || info.format == QLatin1String("jpg")) &&
        fitsFile && fitsPixels) {
        QString err;
        const QByteArray jxl = transcodeJpegToJxl(raw, &err);
        if (!jxl.isEmpty()) {
            // САМОПРОВЕРКА, инвариант A этапа 8. Сравниваем сами байты — это
            // строже отпечатка и столько же стоит.
            const QByteArray back = reconstructJpeg(jxl, &err);
            if (back == raw && jxl.size() < raw.size()) {
                result.route = Route::TranscodedJpeg;
                result.bytes = jxl;
                result.extension = QStringLiteral("jxl");
                result.size = info.size;
                result.bitsPerSample = 8;
                return result;
            }
        }
        // Не сошлось — кладём исходник как есть. Терять байты из-за того, что
        // транскод не удался, нельзя.
        result.route = Route::AsIs;
        result.bytes = raw;
        result.extension = QStringLiteral("jpg");
        result.size = info.size;
        result.bitsPerSample = 8;
        result.message = QStringLiteral("транскод не сошёлся, файл положен как есть");
        return result;
    }

    // --- дальше нужны пиксели --------------------------------------------
    QImage image;
    EncodeMeta meta;
    if (info.format == QLatin1String("tiff") || info.format == QLatin1String("tif")) {
        // Свой читатель: Qt тихо перевирает CIELab, а метаданных не отдаёт
        // вовсе.
        TiffImage tiff;
        QString err;
        if (!readTiff(path, &tiff, &err, qint64(limits.decodeBudgetBytes())))
            return refuse(Refusal::None, QStringLiteral("TIFF не прочитан: %1").arg(err));
        image = tiff.image;
        meta.exif = tiff.exif;
        meta.xmp = tiff.xmp;
        if (!tiff.converted) meta.icc = tiff.icc;
    } else {
        QImageReader reader(path);
        reader.setAutoTransform(true);   // поворот применяем к пикселям
        image = reader.read();
        if (image.isNull())
            return refuse(Refusal::None,
                          QStringLiteral("формат не поддерживается: %1")
                              .arg(reader.errorString()));
        meta = metaFor(raw);
    }

    // --- WebP, влезающий: пробуем перекодировать -------------------------
    if (info.format == QLatin1String("webp") && fitsFile && fitsPixels) {
        const double ratio = losslessRatio(image, limits);
        ImportResult tried = ratio < limits.losslessThreshold
                                 ? losslessPath(image, meta, limits)
                                 : photoPath(image, meta, limits);
        tried.sourceBytes = raw.size();
        tried.encodes += 2;   // проба тоже энкоды
        // Заменяем только при заметном выигрыше: гонять байты ради пяти
        // процентов незачем, а вот потерять качество — запросто.
        if (tried.ok() && tried.bytes.size() * 100 <= raw.size() * 85) return tried;
        result.route = Route::AsIs;
        result.bytes = raw;
        result.extension = QStringLiteral("webp");
        result.size = info.size;
        result.bitsPerSample = info.bitsPerSample;
        result.message = QStringLiteral("перекодирование не дало выигрыша");
        return result;
    }

    // --- PNG и прочее плоское: решает проба -------------------------------
    const bool flatCandidate = info.format == QLatin1String("png") ||
                               info.format == QLatin1String("gif") ||
                               info.format == QLatin1String("bmp");
    if (flatCandidate) {
        const double ratio = losslessRatio(image, limits);
        ImportResult out = ratio < limits.losslessThreshold
                               ? losslessPath(image, meta, limits)
                               : photoPath(image, meta, limits);
        out.sourceBytes = raw.size();
        out.encodes += 2;
        return out;
    }

    // --- всё прочее: путь фото -------------------------------------------
    ImportResult out = photoPath(image, meta, limits);
    out.sourceBytes = raw.size();
    return out;
}

ImportResult importPixels(const QImage& image, const ImportLimits& limits) {
    if (image.isNull()) return refuse(Refusal::Empty);
    const Refusal r = checkSource(image.width(), image.height(),
                                  image.depth() > 32 ? 16 : 8, limits);
    if (r != Refusal::None) return refuse(r);

    // Из буфера обмена приходят и скриншоты, и фотографии, и рисунки — формата
    // у них нет, значит решает только проба.
    const double ratio = losslessRatio(image, limits);
    ImportResult out = ratio < limits.losslessThreshold
                           ? losslessPath(image, EncodeMeta{}, limits)
                           : photoPath(image, EncodeMeta{}, limits);
    out.encodes += 2;
    return out;
}

}  // namespace zametti
