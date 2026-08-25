#include "image_read.h"

#include "jpeg_read.h"
#include "jxl_read.h"
#include "tiff_reader.h"
#include "webp_read.h"

#include "heif_handler.h"

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
    // У HEIF подпись живёт не в начале, а в боксе ftyp; спрашиваем его самого.
    {
        QByteArray copy = head;
        QBuffer buffer(&copy);
        buffer.open(QIODevice::ReadOnly);
        if (HeifHandler::peek(&buffer)) return QStringLiteral("heif");
    }
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


QStringList readableImageExtensions() {
    // Наши читатели (см. sniff выше) плюс то, что умеет сам Qt: png, gif, bmp и
    // прочая мелочь идут через QImage::fromData запасным путём.
    QStringList out{QStringLiteral("jxl"),  QStringLiteral("jpg"), QStringLiteral("jpeg"),
                    QStringLiteral("webp"), QStringLiteral("tif"), QStringLiteral("tiff"),
                    QStringLiteral("png"),  QStringLiteral("gif"), QStringLiteral("bmp"),
                    QStringLiteral("avif"), QStringLiteral("heic"), QStringLiteral("heif")};
    return out;
}

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
        out.frames = readWebpFrameCount(bytes);
        const ImageMeta meta = readImageMeta(std::string_view(bytes.constData(),
                                                              size_t(bytes.size())));
        out.orientation = meta.orientation;
        out.icc = QByteArray::fromStdString(meta.icc);
        return out;
    }

    if (out.format == QLatin1String("heif")) {
        // У HEIF СВОЙ ОБРАБОТЧИК, И СПРАШИВАТЬ НАДО ЕГО. Раньше размеры avif и
        // heic шли к Qt вместе со всеми «чужими» форматами — а Qt про них не
        // знает вовсе (наш читатель плагином не является), отдавал пустой
        // размер, проба выходила невалидной, и ввоз отказывал «формат не
        // поддержан» ещё до нашего декодера. Владелец так и увидел: собрал с
        // поддержкой heif, а avif всё равно не ввозится.
        QByteArray copy = bytes;
        QBuffer buffer(&copy);
        buffer.open(QIODevice::ReadOnly);
        HeifHandler handler;
        handler.setDevice(&buffer);
        out.size = handler.option(QImageIOHandler::Size).toSize();
        // Глубина — из формата кадра, который обещает обработчик: 16 бит на
        // канал у 10- и 12-битных avif (в корпусе владельца такие есть), иначе
        // восемь. По ней считается память до разжатия.
        const QVariant format = handler.option(QImageIOHandler::ImageFormat);
        const auto shape = format.isValid() ? format.value<QImage::Format>() : QImage::Format_Invalid;
        out.bitsPerSample =
            shape == QImage::Format_RGBA64 || shape == QImage::Format_RGBX64 ? 16 : 8;
        out.hasAlpha = shape == QImage::Format_RGBA64 || shape == QImage::Format_RGBA8888;
        out.frames = 1;
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
    // Кадров у неанимированного формата бывает и ноль, и минус один: у каждого
    // читателя свой ответ. Наружу отдаём «хотя бы один».
    out.frames = qMax(1, reader.imageCount());
    return out;
}

ImageProbe probeImageFile(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    const QByteArray head = file.read(kProbeBytes);
    const QString format = sniff(head);

    if (format == QLatin1String("tiff")) {
        // У TIFF каталог тегов может лежать в конце файла, и по началу его не
        // прочесть. Спрашиваем ТОЛЬКО ТЕГИ: полное чтение здесь разжимало бы
        // всю картинку на каждой раскладке — на этом я уже обжёгся.
        ImageProbe out;
        out.format = format;
        TiffHeader head;
        QString error;
        if (readTiffHeader(path, &head, &error)) {
            out.size = head.size;
            // Глубина РАБОЧЕГО кадра, а не файла: по ней считается память.
            out.bitsPerSample = head.wideFrame ? 16 : head.bitsPerSample;
            out.hasAlpha = head.hasAlpha;
            out.icc = head.icc;
        }
        // Размеры не дались (DNG и прочее сырьё в оболочке TIFF) — спрашиваем
        // Qt: читать это будет он же, значит и мерить ему.
        if (!out.valid()) out.size = QImageReader(path).size();
        return out;
    }

    // СНАЧАЛА ПО КУСКУ, И ТОЛЬКО ЕСЛИ НЕ ВЫШЛО — ПО ВСЕМУ ФАЙЛУ.
    //
    // Кусок в 64 КБ покрывает подавляющее большинство: у jxl, jpeg и webp всё
    // нужное лежит в начале. Но не всегда — и на этом я обжёгся. У снимка с
    // телефона (vivo, Display P3) цветовой профиль в маркере APP2 длиннее
    // куска, разбор шапки спотыкался о обрыв, размер выходил пустым, и ввоз
    // молча ОТКАЗЫВАЛ картинке, которую раньше принимал.
    //
    // Порядок именно такой, а не «всегда целиком»: пробу зовут на каждой
    // раскладке для каждой картинки, и читать ради неё сто мегабайт нельзя.
    const ImageProbe quick = probeImage(head);
    if (quick.valid()) return quick;

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
        if (!readTiff(path, &tiff, &error, request.memoryLimitBytes)) {
            // СВОЙ ЧИТАТЕЛЬ НЕ СМОГ — ПУСТЬ ПРОБУЕТ Qt. В оболочку TIFF
            // заворачивают не только картинки: DNG с телефона несёт сырые
            // отсчёты матрицы, и наш читатель честно отказывается их толковать,
            // а плагин Qt достаёт из файла готовое превью. Отказать человеку
            // было бы хуже: мы не опираемся на чужие плагины, но и не отвергаем
            // назло то, что они умеют.
            return QImage(path);
        }
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
