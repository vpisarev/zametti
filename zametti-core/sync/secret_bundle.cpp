// SecretBundle: формат свёртка и индекса. Подробности — в заголовке.

#include "secret_bundle.h"

#include <QCborArray>
#include <QCborMap>
#include <QCborValue>

namespace zametti {
namespace {

constexpr qint64 kVersion = 1;

// Имена слотов — общий словарь трёх воплощений (атрибут what у dbus, кусок
// TargetName у wincred): одно хранилище, открытое на двух системах, зовёт
// секреты одинаково.
QString slotName(SecretStore::Secret which) {
    switch (which) {
        case SecretStore::Secret::Key: return QStringLiteral("key");
        case SecretStore::Secret::ServerPassword: return QStringLiteral("webdav");
        case SecretStore::Secret::EncryptionPassword:
            return QStringLiteral("password");
    }
    return QString();
}

}  // namespace

bool SecretBundle::parse(const QByteArray& bytes) {
    const QCborValue root = QCborValue::fromCbor(bytes);
    if (!root.isMap()) return false;
    const QCborMap map = root.toMap();
    // Версию ВЫШЕ своей отвергаем: переписать её свёрток значило бы молча
    // выкинуть то, чего эта версия формата не знает (см. шапку заголовка).
    if (map.value(QStringLiteral("v")).toInteger(-1) != kVersion) return false;
    const QCborValue storesValue = map.value(QStringLiteral("stores"));
    if (!storesValue.isMap()) return false;

    QMap<QString, QMap<QString, QByteArray>> parsed;
    const QCborMap stores = storesValue.toMap();
    for (auto it = stores.constBegin(); it != stores.constEnd(); ++it) {
        if (!it.key().isString() || !it.value().isMap()) return false;
        // «slots» звать нельзя: это макрос Qt.
        QMap<QString, QByteArray> slotValues;
        const QCborMap slotMap = it.value().toMap();
        for (auto st = slotMap.constBegin(); st != slotMap.constEnd(); ++st) {
            if (!st.key().isString() || !st.value().isByteArray()) return false;
            const QByteArray value = st.value().toByteArray();
            if (!value.isEmpty()) slotValues.insert(st.key().toString(), value);
        }
        if (!slotValues.isEmpty()) parsed.insert(it.key().toString(), slotValues);
    }
    stores_ = parsed;
    return true;
}

QByteArray SecretBundle::toBytes() const {
    QCborMap stores;
    for (auto it = stores_.constBegin(); it != stores_.constEnd(); ++it) {
        QCborMap slotValues;
        for (auto st = it.value().constBegin(); st != it.value().constEnd(); ++st)
            slotValues.insert(st.key(), st.value());
        stores.insert(it.key(), slotValues);
    }
    QCborMap root;
    root.insert(QStringLiteral("v"), kVersion);
    root.insert(QStringLiteral("stores"), stores);
    return QCborValue(root).toCbor();
}

QByteArray SecretBundle::value(const QString& storeId,
                               SecretStore::Secret which) const {
    return stores_.value(storeId).value(slotName(which));
}

void SecretBundle::put(const QString& storeId, SecretStore::Secret which,
                       const QByteArray& value) {
    if (value.isEmpty()) {
        auto it = stores_.find(storeId);
        if (it == stores_.end()) return;
        it->remove(slotName(which));
        if (it->isEmpty()) stores_.erase(it);
        return;
    }
    stores_[storeId].insert(slotName(which), value);
}

QByteArray SecretBundle::index() const {
    QCborMap map;
    for (auto it = stores_.constBegin(); it != stores_.constEnd(); ++it) {
        QCborArray names;
        for (auto st = it.value().constBegin(); st != it.value().constEnd(); ++st)
            names.append(st.key());
        map.insert(it.key(), names);
    }
    return QCborValue(map).toCbor();
}

bool SecretBundle::indexHas(const QByteArray& index, const QString& storeId,
                            SecretStore::Secret which) {
    const QCborValue root = QCborValue::fromCbor(index);
    if (!root.isMap()) return false;
    const QCborValue names = root.toMap().value(storeId);
    if (!names.isArray()) return false;
    return names.toArray().contains(QCborValue(slotName(which)));
}

}  // namespace zametti
