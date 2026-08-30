// ZStorageManager: список хранилищ устройства и факты строк. Подробности — в
// заголовке.

#include "zstorage_manager.h"

#include "secret_store.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonObject>

namespace zametti {

ZStorageManager::ZStorageManager(std::shared_ptr<SecretStore> secrets)
    : secrets_(std::move(secrets)) {}

void ZStorageManager::setSecrets(std::shared_ptr<SecretStore> secrets) {
    secrets_ = std::move(secrets);
    factsCache_.clear();
}

QString ZStorageManager::canonicalRoot(const QString& root) {
    if (root.isEmpty()) return {};
    return QDir::cleanPath(QFileInfo(root).absoluteFilePath());
}

void ZStorageManager::remember(const ZStorage::Config& entry) {
    if (entry.root.isEmpty()) return;
    ZStorage::Config kept = entry;
    kept.root = canonicalRoot(entry.root);
    factsCache_.remove(kept.root);
    storeIds_.remove(kept.root);
    for (ZStorage::Config& e : stores_) {
        if (e.root == kept.root) {
            // Имя могло не приехать (звали без чтения корня) — прежнее
            // дороже пустого.
            if (kept.name.isEmpty()) kept.name = e.name;
            e = kept;
            return;
        }
    }
    stores_.append(kept);
}

void ZStorageManager::forget(const QString& root) {
    const QString key = canonicalRoot(root);
    factsCache_.remove(key);
    storeIds_.remove(key);
    for (qsizetype i = stores_.size(); i-- > 0;)
        if (stores_[i].root == key) stores_.removeAt(i);
}

ZStorage::Config ZStorageManager::storeFor(const QString& root) const {
    const QString key = canonicalRoot(root);
    for (const ZStorage::Config& e : stores_)
        if (e.root == key) return e;
    return {};
}

QJsonArray ZStorageManager::storesToJson() const {
    QJsonArray out;
    for (const ZStorage::Config& e : stores_) out.append(e.entryJson());
    return out;
}

void ZStorageManager::storesFromJson(const QJsonArray& stores) {
    stores_.clear();
    factsCache_.clear();
    storeIds_.clear();
    for (const QJsonValue& v : stores) {
        ZStorage::Config entry;
        entry.parse(v.toObject());
        // Через remember, а не напрямую: канонизация и дедупликация — одни
        // на чтение и на запись.
        remember(entry);
    }
}

QString ZStorageManager::storeIdOf(const QString& key) {
    const auto it = storeIds_.constFind(key);
    if (it != storeIds_.constEnd()) return *it;
    const QString id = ZStorage(key).identity().storeId();
    storeIds_.insert(key, id);
    return id;
}

ZStorageManager::Facts ZStorageManager::facts(const QString& folder) {
    const QString key = canonicalRoot(folder);
    if (key.isEmpty()) return {};
    const auto cached = factsCache_.constFind(key);
    if (cached != factsCache_.constEnd()) return *cached;

    Facts out;
    out.kind = ZStorage::inspect(key);
    if (out.kind == ZStorage::DirKind::Store) {
        out.stats = ZStorage::localSummary(key);
        const QString id = storeIdOf(key);
        if (!id.isEmpty() && secrets_ != nullptr && secrets_->available()) {
            // Спросить, НЕ читая (has): чтение секрета на переключение строки
            // поднимало бы системный вопрос связки на маке.
            const auto known = [&](SecretStore::Secret which) {
                return secrets_->has(id, which) ? Known::Yes : Known::No;
            };
            out.key = known(SecretStore::Secret::Key);
            out.serverPassword = known(SecretStore::Secret::ServerPassword);
            out.encryptionPassword = known(SecretStore::Secret::EncryptionPassword);
        }
        // Строка списка носит сводку с собой (Config::local — показ, не
        // формат): диалогу не приходится спрашивать её вторым путём.
        for (ZStorage::Config& e : stores_)
            if (e.root == key) e.local = out.stats;
    }
    factsCache_.insert(key, out);
    return out;
}

void ZStorageManager::refresh(const QString& root) {
    const QString key = canonicalRoot(root);
    factsCache_.remove(key);
    storeIds_.remove(key);
}

}  // namespace zametti
