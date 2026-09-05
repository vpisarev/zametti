#include "image_insert.h"

#include "exif.h"

#include <QDateTime>

#include <random>

#include "import.h"
#include "note_id.h"
#include "settings.h"

#include <QDir>
#include <QFileInfo>

namespace zametti {

namespace {

// Случайная часть id. Своя, а не из ядра: там она спрятана в newNoteId, а нам
// нужна отдельно — секунды мы задаём сами.
std::uint64_t systemRandomForImages() {
    std::random_device rd;
    return (std::uint64_t(rd()) << 32) | rd();
}

// Запись строго на свежее имя — тем же writeNewFile, что и заметки: сам
// createNoteFile нам не годится, он жёстко пишет ".md".
// Двойник: вложение с тем же префиксом имени, у которого СЖАТЫЕ ДАННЫЕ те же.
//
// Кандидатов ищем перебором имён — префикс кодирует секунду съёмки, и их
// оказывается ноль-один, редко два. Никакого кэша заводить не надо: каталог
// хранилища плоский, а сравнение доходит до чтения файла лишь для тех
// немногих, у кого совпал префикс.
//
// Сравниваются ДАННЫЕ, а не файлы: подпись к снимку уезжает в метаданные, и
// сравнение целиком плодило бы двойников ровно у подписанных картинок.
QString findTwin(const QString& dir, const QString& prefix, const QString& suffix,
                 const QByteArray& fresh) {
    const QStringList candidates =
        QDir(dir).entryList({prefix + QStringLiteral("*.") + suffix}, QDir::Files);
    for (const QString& name : candidates) {
        QFile file(QDir(dir).filePath(name));
        if (!file.open(QIODevice::ReadOnly)) continue;
        const QByteArray old = file.readAll();
        if (sameCompressedData(
                std::string_view(old.constData(), size_t(old.size())),
                std::string_view(fresh.constData(), size_t(fresh.size()))))
            return name;
    }
    return {};
}

QString writeFresh(const QString& dir, const QString& suffix, const QByteArray& bytes,
                   QString* error, qint64 takenAt = 0, bool* duplicate = nullptr) {
    // Имя чеканится от ВРЕМЕНИ СЪЁМКИ, а не от «сейчас»: тогда повторный
    // ввоз того же снимка даёт тот же префикс id (первые восемь знаков —
    // секунды), и двойник ищется простым перебором имён, без кэша. У
    // снимка без даты (буфер обмена, скриншот) берётся «сейчас» — брать
    // больше неоткуда, и двойников у таких не бывает по построению.
    const auto mint = [takenAt] {
        return takenAt > 0 ? makeNoteId(std::uint64_t(takenAt), systemRandomForImages())
                           : newNoteId();
    };
    // Двойник ищется ОДИН раз, до записи: повторные попытки бывают только от
    // коллизии имён, а она к содержимому отношения не имеет.
    if (takenAt > 0) {
        const std::string id = mint();
        const QString twin =
            findTwin(QDir::cleanPath(dir), QString::fromStdString(id).left(8), suffix, bytes);
        if (!twin.isEmpty()) {
            // Файла не заводим вовсе — ссылаемся на тот, что уже лежит.
            if (duplicate != nullptr) *duplicate = true;
            return twin;
        }
    }
    // The O_EXCL loop with a fresh id per collision lives in the core, one
    // for notes, pictures and books alike (createAttachmentFile).
    const std::string name = createAttachmentFile(QDir::cleanPath(dir).toUtf8().toStdString(),
                                                  suffix.toStdString(), bytes.constData(),
                                                  std::size_t(bytes.size()), mint);
    if (name.empty()) {
        *error = QStringLiteral("could not write an attachment file in %1").arg(dir);
        return {};
    }
    return QString::fromStdString(name);
}

StoredImage finish(const ImportResult& r, const QString& storeDir, const QString& alt,
                   qint64 takenAt = 0) {
    StoredImage out;
    if (!r.ok()) {
        out.error = r.message.isEmpty() ? QStringLiteral("image not accepted") : r.message;
        return out;
    }
    out.fileName = writeFresh(storeDir, r.extension, r.bytes, &out.error, takenAt, &out.duplicate);
    if (out.fileName.isEmpty()) return out;

    out.alt = alt;
    out.width = r.size.width;
    out.height = r.size.height;
    out.message = QStringLiteral("%1, %2×%3").arg(QString::fromUtf8(routeName(r.route)))
                      .arg(r.size.width).arg(r.size.height);
    if (!r.message.isEmpty()) out.message += QStringLiteral(" — ") + r.message;
    return out;
}

// Когда снимок сделан, в секундах эпохи. Порядок источников — от самого
// верного к запасному: EXIF DateTimeOriginal (момент съёмки), XMP
// xmp:CreateDate (сканы и экспорт из редакторов), дата создания файла, а если
// её файловая система не хранит — время правки.
qint64 shotSeconds(const QString& path) {
    QFile file(path);
    if (file.open(QIODevice::ReadOnly)) {
        // Читаем НЕ ВЕСЬ файл: метаданные лежат в начале, а снимок бывает и на
        // сорок мегабайт. Двух мегабайт хватает с большим запасом.
        const QByteArray head = file.read(2 * 1024 * 1024);
        const ImageMeta meta =
            readImageMeta(std::string_view(head.constData(), size_t(head.size())));
        QString when = QString::fromStdString(exifDateTaken(meta.exif));
        if (when.isEmpty()) when = QString::fromStdString(xmpCreateDate(meta.xmp));
        const QDateTime taken = QDateTime::fromString(when, Qt::ISODate);
        // Зоны в EXIF нет, время местное — так его и читаем.
        if (taken.isValid()) return QDateTime(taken.date(), taken.time()).toSecsSinceEpoch();
    }
    const QFileInfo info(path);
    const QDateTime born = info.birthTime();
    if (born.isValid()) return born.toSecsSinceEpoch();
    const QDateTime changed = info.lastModified();
    return changed.isValid() ? changed.toSecsSinceEpoch() : 0;
}

}  // namespace

StoredImage storeImageFile(const QString& sourcePath, const QString& storeDir,
                           const ImportLimits& limits) {
    // Alt — базовое имя исходника, без расширения: имя файла в хранилище
    // бессмысленно, и единственное место, где человеческое имя переживает
    // вставку, — это alt.
    const QString alt = QFileInfo(sourcePath).completeBaseName();
    return finish(importImage(sourcePath, limits), storeDir, alt, shotSeconds(sourcePath));
}

StoredImage storeImagePixels(const QImage& image, const QString& storeDir,
                             const ImportLimits& limits) {
    // У битмапа из буфера имени нет, и выдумывать его нельзя: alt должен
    // описывать картинку, а «Вставленное изображение» не описывает ничего.
    return finish(importPixels(image, limits), storeDir, QString());
}

ImportLimits importLimitsFrom(const ZSettings::Images& s) {
    ImportLimits limits;
    limits.maxSize = s.maxImportedImageSize();
    limits.quality = s.photoQuality();
    limits.losslessThreshold = s.losslessThreshold();
    limits.maxBitsPerChannel = s.maxBitsPerChannel();
    limits.maxDecodeMemoryMb = s.maxDecodeMemoryMb();
    return limits;
}

ImportLimits deletedImageLimitsFrom(const ZSettings::Images& s) {
    ImportLimits limits = importLimitsFrom(s);
    limits.maxSize = s.maxDeletedImageSize();
    return limits;
}

QString imageMarkdown(const StoredImage& stored) {
    if (!stored.ok()) return {};
    QString alt = stored.alt;
    // Скобки в alt закрыли бы ссылку раньше времени. Не экранируем, а убираем:
    // alt — это подпись для человека, и обратные косые в ней смотрелись бы
    // хуже, чем отсутствие скобок.
    alt.replace(QLatin1Char('['), QLatin1Char('('));
    alt.replace(QLatin1Char(']'), QLatin1Char(')'));
    return QStringLiteral("![%1](%2)").arg(alt, stored.fileName);
}

}  // namespace zametti
