#include "image_insert.h"

#include "import.h"
#include "note_id.h"
#include "settings.h"

#include <QDir>
#include <QFileInfo>

#include <fcntl.h>
#include <unistd.h>

namespace zametti {

namespace {

// Запись строго на свежее имя. Тот же приём, что и у createNoteFile для
// заметок, и по той же причине: O_EXCL делает «проверить и создать» одним
// действием ядра. Отдельной функции там нет — та жёстко пишет ".md".
QString writeFresh(const QString& dir, const QString& suffix, const QByteArray& bytes,
                   QString* error) {
    const QByteArray dirUtf8 = QDir::cleanPath(dir).toUtf8();
    for (int attempt = 0; attempt < 64; ++attempt) {
        const QString name = QString::fromStdString(newNoteId()) + QLatin1Char('.') + suffix;
        const QByteArray path = dirUtf8 + '/' + name.toUtf8();

        const int fd = ::open(path.constData(), O_WRONLY | O_CREAT | O_EXCL, 0644);
        if (fd < 0) {
            if (errno == EEXIST) continue;   // редчайшая коллизия — берём другой id
            *error = QStringLiteral("не удалось создать файл вложения в %1").arg(dir);
            return {};
        }
        qsizetype at = 0;
        bool ok = true;
        while (at < bytes.size()) {
            const ssize_t n = ::write(fd, bytes.constData() + at, size_t(bytes.size() - at));
            if (n < 0) {
                ok = false;
                break;
            }
            at += qsizetype(n);
        }
        if (::close(fd) != 0) ok = false;
        if (ok) return name;

        // Недописанное убираем: половина картинки в хранилище хуже, чем её
        // отсутствие, — она и покажется сломанной, и место займёт.
        ::unlink(path.constData());
        *error = QStringLiteral("вложение не записалось целиком");
        return {};
    }
    *error = QStringLiteral("не нашлось свободного имени для вложения");
    return {};
}

StoredImage finish(const ImportResult& r, const QString& storeDir, const QString& alt) {
    StoredImage out;
    if (!r.ok()) {
        out.error = r.message.isEmpty() ? QStringLiteral("картинка не принята") : r.message;
        return out;
    }
    out.fileName = writeFresh(storeDir, r.extension, r.bytes, &out.error);
    if (out.fileName.isEmpty()) return out;

    out.alt = alt;
    out.width = r.size.width;
    out.height = r.size.height;
    out.message = QStringLiteral("%1, %2×%3").arg(QString::fromUtf8(routeName(r.route)))
                      .arg(r.size.width).arg(r.size.height);
    if (!r.message.isEmpty()) out.message += QStringLiteral(" — ") + r.message;
    return out;
}

}  // namespace

StoredImage storeImageFile(const QString& sourcePath, const QString& storeDir,
                           const ImportLimits& limits) {
    // Alt — базовое имя исходника, без расширения: имя файла в хранилище
    // бессмысленно, и единственное место, где человеческое имя переживает
    // вставку, — это alt.
    const QString alt = QFileInfo(sourcePath).completeBaseName();
    return finish(importImage(sourcePath, limits), storeDir, alt);
}

StoredImage storeImagePixels(const QImage& image, const QString& storeDir,
                             const ImportLimits& limits) {
    // У битмапа из буфера имени нет, и выдумывать его нельзя: alt должен
    // описывать картинку, а «Вставленное изображение» не описывает ничего.
    return finish(importPixels(image, limits), storeDir, QString());
}

ImportLimits limitsFromSettings() {
    const Appearance::Images& s = appearance().images;
    ImportLimits limits;
    limits.maxSize = s.maxImportedImageSize;
    limits.maxFileSizeMb = s.maxImportedImageFileSizeMb;
    limits.quality = s.photoQuality;
    limits.losslessThreshold = s.losslessThreshold;
    limits.maxBitsPerChannel = s.maxBitsPerChannel;
    limits.maxDecodeMemoryMb = s.maxDecodeMemoryMb;
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
