#include "image_facts.h"

#include <QDateTime>
#include <QFileInfo>
#include <QHash>
#include <QImageReader>

namespace zametti {
namespace {

// Ключ кэша — не только путь: вложение могло смениться на диске (синхронизация,
// правка руками). Время правки вместе с размером ловит и то и другое, а стоят
// они одного stat, который QFileInfo всё равно делает.
struct Key {
    qint64 modified = 0;
    qint64 bytes = 0;
};

struct Entry {
    Key key;
    ImageFacts facts;
};

QHash<QString, Entry>& cache() {
    static QHash<QString, Entry> table;
    return table;
}

}  // namespace

ImageFacts imageFacts(const QString& absolutePath) {
    ImageFacts out;
    if (absolutePath.isEmpty()) return out;
    out.path = absolutePath;
    const QFileInfo file(absolutePath);
    out.name = file.fileName();

    if (!file.exists()) {
        // Кэшировать «нет файла» нельзя: он ровно за этим и появится.
        out.valid = true;
        return out;
    }
    out.exists = true;

    const Key key{file.lastModified().toMSecsSinceEpoch(), file.size()};
    const auto found = cache().constFind(absolutePath);
    if (found != cache().constEnd() && found->key.modified == key.modified &&
        found->key.bytes == key.bytes)
        return found->facts;

    out.bytes = file.size();
    QImageReader reader(absolutePath);
    out.size = reader.size();
    out.format = QString::fromLatin1(reader.format());
    // Кадров у неанимированного формата бывает и ноль, и минус один: у каждого
    // читателя свой ответ. Наружу отдаём «хотя бы один».
    out.frames = qMax(1, reader.imageCount());
    out.valid = true;

    cache().insert(absolutePath, Entry{key, out});
    return out;
}

int imageFactsCacheSize() { return int(cache().size()); }

void clearImageFactsCache() { cache().clear(); }

}  // namespace zametti
