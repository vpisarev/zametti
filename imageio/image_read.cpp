#include "image_read.h"

#include "jpeg_read.h"
#include "jxl_read.h"
#include "tiff_reader.h"
#include "webp_read.h"

#ifdef ZAMETTI_HAVE_HEIF
#include "heif_handler.h"
#endif

#include <QBuffer>
#include <QColorSpace>
#include <QFile>
#include <QImageReader>
#include <QTransform>

namespace zametti {

namespace {

// Развилка по подписи. Порядок неважен: подписи не пересекаются.
QString sniff(const QByteArray& head) {
    if (looksLikeJxl(head))  return QStringLiteral("jxl");
    if (looksLikeJpeg(head)) return QStringLiteral("jpeg");
    if (looksLikeWebp(head)) return QStringLiteral("webp");
    if (looksLikeTiff(head)) return QStringLiteral("tiff");
#ifdef ZAMETTI_HAVE_HEIF
    // У HEIF подпись живёт не в начале, а в боксе ftyp; спрашиваем его самого.
    {
        QByteArray copy = head;
        QBuffer buffer(&copy);
        buffer.open(QIODevice::ReadOnly);
        if (HeifHandler::peek(&buffer)) return QStringLiteral("heif");
    }
#endif
    return {};
}

// Влезает ли разжатая копия в отведённое. Спрашиваем ДО разжатия.
bool fitsBudget(const ImageProbe& probe, qint64 limit) {
    if (limit <= 0) return true;
    return probe.decodedBytes() <= limit;
}

// Наибольшая сторона у Qt тоже имеет потолок, и он не наш: QImage просто
// вернёт пустую. Спросить его заранее дешевле, чем узнать после.
bool fitsQtAllocation(const ImageProbe& probe) {
    const qint64 limitMb = qint64(QImageReader::allocationLimit());
    if (limitMb <= 0) return true;   // потолок снят самим приложением
    const qint64 full = qint64(probe.size.width()) * probe.size.height() * 4;
    return full <= limitMb * 1024 * 1024;
}

}  // namespace

qint64 ImageProbe::decodedBytes() const {
    if (size.isEmpty()) return 0;
    const qint64 perPixel = bitsPerSample > 8 ? 8 : 4;   // RGBA64 либо RGBA8888
    return qint64(size.width()) * size.height() * perPixel;
}

QString sniffImageFormat(const QByteArray& head) { return sniff(head); }

ImageProbe probeImage(const QByteArray& bytes) {
    ImageProbe out;
    out.format = sniff(bytes);

    if (out.format == QLatin1String("jxl")) {
        const JxlHeader head = readJxlHeader(bytes);
        out.size = head.size;
        out.bitsPerSample = head.bitsPerSample;
        out.hasAlpha = head.hasAlpha;
        out.orientation = head.orientation;
        out.icc = head.icc;
        return out;
    }
    if (out.format == QLatin1String("jpeg")) {
        const JpegHeader head = readJpegHeader(bytes);
        out.size = head.size;
        out.bitsPerSample = 8;   // в файле всегда восемь; глубже отдаёт декодер
        out.hasAlpha = false;
        // Ориентация JPEG живёт в EXIF, а не в шапке кадра.
        const ImageMeta meta = readImageMeta(std::string_view(bytes.constData(),
                                                              size_t(bytes.size())));
        out.orientation = meta.orientation;
        out.icc = QByteArray::fromStdString(meta.icc);
        return out;
    }
    if (out.format == QLatin1String("webp")) {
        out.size = readWebpSize(bytes);
        const ImageMeta meta = readImageMeta(std::string_view(bytes.constData(),
                                                              size_t(bytes.size())));
        out.orientation = meta.orientation;
        out.icc = QByteArray::fromStdString(meta.icc);
        return out;
    }

    // Остальные — у Qt: он умеет спросить размер, не разжимая, и для чужих
    // форматов это ровно то, что нужно. TIFF сюда тоже попадает: его шапку
    // читает свой путь по файлу, а не по буферу.
    QByteArray copy = bytes;
    QBuffer buffer(&copy);
    buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer);
    out.size = reader.size();
    return out;
}

ImageProbe probeImageFile(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    const QByteArray head = file.read(kProbeBytes);
    const QString format = sniff(head);

    if (format == QLatin1String("tiff")) {
        // У TIFF каталог тегов может лежать в конце файла, и по началу его не
        // прочесть. Свой читатель ходит по файлу сам.
        ImageProbe out;
        out.format = format;
        TiffImage tiff;
        QString error;
        if (readTiff(path, &tiff, &error, 0)) {
            out.size = tiff.image.size();
            out.bitsPerSample = tiff.bitsPerSample;
            out.hasAlpha = tiff.image.hasAlphaChannel();
            out.icc = tiff.icc;
        }
        return out;
    }

    // JXL, JPEG и WebP держат всё нужное в начале — хватает прочитанного куска.
    // Чужим форматам и HEIF отдаём файл целиком: у них шапка бывает где угодно.
    if (format == QLatin1String("jxl") || format == QLatin1String("jpeg") ||
        format == QLatin1String("webp"))
        return probeImage(head);

    file.seek(0);
    return probeImage(file.readAll());
}

QImage applyOrientation(QImage image, Orientation orientation) {
    if (image.isNull() || orientation == Orientation::Normal) return image;
    QTransform t;
    switch (orientation) {
        case Orientation::Normal: return image;
        case Orientation::FlipHorizontal: return image.flipped(Qt::Horizontal);
        case Orientation::Rotate180:      t.rotate(180); break;
        case Orientation::FlipVertical:   return image.flipped(Qt::Vertical);
        case Orientation::Transpose:
            t.rotate(90);
            image = image.flipped(Qt::Horizontal);
            break;
        case Orientation::Rotate90:       t.rotate(90);  break;
        case Orientation::AntiTranspose:
            t.rotate(270);
            image = image.flipped(Qt::Horizontal);
            break;
        case Orientation::Rotate270:      t.rotate(270); break;
    }
    return image.transformed(t, Qt::SmoothTransformation);
}

QImage decodeImage(const QByteArray& bytes, const DecodeRequest& request) {
    const ImageProbe probe = probeImage(bytes);
    if (probe.valid() && !fitsQtAllocation(probe)) return {};
    if (probe.valid() && !fitsBudget(probe, request.memoryLimitBytes)) return {};

    QImage out;
    if (probe.format == QLatin1String("jxl")) {
        out = decodeJxl(bytes);
    } else if (probe.format == QLatin1String("jpeg")) {
        out = decodeJpeg(bytes, request.maxSize,
                         request.deep ? JpegDepth::Sixteen : JpegDepth::Eight);
    } else if (probe.format == QLatin1String("webp")) {
        out = decodeWebp(bytes);
#ifdef ZAMETTI_HAVE_HEIF
    } else if (probe.format == QLatin1String("heif")) {
        // Обработчик HEIF оставлен обработчиком: он уже написан, работает и
        // разбирать его ради единообразия незачем. Плагином он при этом БОЛЬШЕ
        // НЕ ЯВЛЯЕТСЯ — мы зовём его напрямую, минуя реестр Qt.
        QByteArray copy = bytes;
        QBuffer buffer(&copy);
        buffer.open(QIODevice::ReadOnly);
        HeifHandler handler;
        handler.setDevice(&buffer);
        if (!handler.read(&out)) out = QImage();
#endif
    } else {
        // Чужой формат: пусть пробует Qt. Получится — хорошо, не получится —
        // строка останется строкой, как и всякий файл, который мы не поняли.
        out = QImage::fromData(bytes);
    }

    if (out.isNull()) return out;

    // ПРОФИЛЬ. Забыть его — самая тихая из потерь: картинка читается, размеры
    // сходятся, и только цвета не те. Треть снимков в каталогах владельца —
    // Display P3, и без профиля они показывались бы как sRGB, то есть блёкло.
    //
    // Нашёл это набор, а не глаз: SSIMULACRA2 против плагина Qt давала 84–89
    // РОВНО у всех jpeg подряд, включая файл без прореживания цветности, где
    // расходиться декодерам почти негде. Постоянство оценки и выдало причину —
    // дело было не в декоде, а в цвете. Тот же приём, что на этапе 8.
    //
    // Читатели, которые ставят профиль сами (jxl, tiff), сюда не попадают: у
    // них он уже стоит, и переклеивать его поверх нельзя — у jxl там целый
    // разбор с переводом пространства.
    if (!out.colorSpace().isValid() && !probe.icc.isEmpty()) {
        const QColorSpace space = QColorSpace::fromIccProfile(probe.icc);
        if (space.isValid()) out.setColorSpace(space);
    }

    if (request.applyOrientation) out = applyOrientation(std::move(out), probe.orientation);
    return out;
}

QImage decodeImageFile(const QString& path, const DecodeRequest& request) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    const QByteArray head = file.peek(kProbeBytes);

    if (sniff(head) == QLatin1String("tiff")) {
        TiffImage tiff;
        QString error;
        if (!readTiff(path, &tiff, &error, request.memoryLimitBytes)) return {};
        QImage out = std::move(tiff.image);
        if (request.applyOrientation) {
            const ImageMeta meta = readImageMeta(
                std::string_view(tiff.exif.constData(), size_t(tiff.exif.size())));
            out = applyOrientation(std::move(out), meta.orientation);
        }
        return out;
    }
    return decodeImage(file.readAll(), request);
}

}  // namespace zametti
