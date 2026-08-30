#include "cloud_store.h"

namespace zametti {

void CloudStore::getMany(const QStringList& names, QHash<QString, Fetched>* out) {
    Q_ASSERT(out != nullptr);
    for (const QString& name : names) {
        Fetched one;
        one.ok = get(name, &one.bytes, nullptr, &one.error);
        out->insert(name, one);
    }
}

bool CloudStore::removeTree(QString* error) {
    // Дефолт — по одному: у плоского пространства имён другого общего пути
    // нет. Пустое облако (или недостижимый листинг пустого) — решает листинг.
    QVector<Entry> listing;
    if (!list(&listing, error)) return false;
    for (const Entry& e : listing)
        if (!del(e.name, error)) return false;
    return true;
}

bool CloudStore::putIfMatch(const QString& name, const QByteArray& bytes,
                             const QString& expectedEtag, QString* etag,
                             bool* preconditionFailed, QString* error) {
    // Дефолт — молчаливая деградация: адаптер, не умеющий условной заливки,
    // заливает обычным способом. Честнее, чем отказ: гонку ловит не только
    // If-Match, а весь ярус слияния (union по записям), и она не теряет
    // данных даже без него.
    Q_UNUSED(expectedEtag);
    if (preconditionFailed != nullptr) *preconditionFailed = false;
    return put(name, bytes, etag, error);
}

}  // namespace zametti
