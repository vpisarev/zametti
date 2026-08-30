// BundledSecrets: правила одной записи. Подробности — в заголовке.

#include "bundled_secrets.h"

namespace zametti {

bool BundledSecrets::load(QString* error) {
    // СВЁРТОК ЧИТАЕТСЯ ОДИН РАЗ ЗА ПРОГОН. Вопрос системы, если он и будет,
    // случится единожды; пишем свёрток мы сами, поэтому кэш не устаревает.
    if (loaded_) return true;
    QByteArray data;
    bool found = false;
    if (!readBundle(&data, &found, error)) return false;
    if (found && !bundle_.parse(data)) {
        if (error != nullptr)
            *error = QStringLiteral("the secret store is not readable");
        return false;
    }
    loaded_ = true;
    return true;
}

QByteArray BundledSecrets::take(const QString& storeId, Secret which, QString* error) {
    if (!load(error)) return {};
    return bundle_.value(storeId, which);
}

bool BundledSecrets::keep(const QString& storeId, Secret which, const QByteArray& value,
                          QString* error) {
    // ЧИТАЕМ ПЕРЕД ЗАПИСЬЮ — иначе затрём чужие секреты в той же записи.
    // Первой записи читать нечего: её ещё нет, и вопроса тоже.
    if (!load(error)) return false;
    bundle_.put(storeId, which, value);
    return writeBundle(bundle_.toBytes(), bundle_.index(), error);
}

bool BundledSecrets::has(const QString& storeId, Secret which) {
    // ПО УКАЗАТЕЛЮ, А НЕ ПО СЕКРЕТУ. Показ спрашивает это на всякое
    // переключение строки, и читать ради него секреты нельзя.
    return SecretBundle::indexHas(readIndex(), storeId, which);
}

bool BundledSecrets::loadKey(const QString& storeId, Keyfile* out, QString* error) {
    Q_ASSERT(out != nullptr);
    const QByteArray bytes = take(storeId, Secret::Key, error);
    if (bytes.isEmpty()) {
        if (error != nullptr && error->isEmpty())
            *error = QStringLiteral("no key for %1 in the secret store").arg(storeId);
        return false;
    }
    const Keyfile made = Keyfile::fromLiveKey(storeId, bytes);
    if (!made.hasKey()) {
        if (error != nullptr)
            *error = QStringLiteral("the secret store holds no %1-byte key for %2")
                         .arg(Keyfile::kKeyBytes)
                         .arg(storeId);
        return false;
    }
    *out = made;
    return true;
}

bool BundledSecrets::storeKey(const Keyfile& keyfile, QString* error) {
    if (!keyfile.hasKey()) {
        if (error != nullptr) *error = QStringLiteral("no live key to store");
        return false;
    }
    return keep(keyfile.storeId(), Secret::Key, keyfile.key(), error);
}

bool BundledSecrets::clearKey(const QString& storeId, QString* error) {
    return keep(storeId, Secret::Key, QByteArray(), error);
}

QString BundledSecrets::serverPassword(const QString& storeId, QString* error) {
    const QByteArray bytes = take(storeId, Secret::ServerPassword, error);
    if (bytes.isEmpty()) {
        if (error != nullptr && error->isEmpty())
            *error = QStringLiteral("no server password for %1 in the secret store")
                         .arg(storeId);
        return QString();
    }
    return QString::fromUtf8(bytes);
}

bool BundledSecrets::setServerPassword(const QString& storeId, const QString& password,
                                       QString* error) {
    return keep(storeId, Secret::ServerPassword, password.toUtf8(), error);
}

bool BundledSecrets::clearServerPassword(const QString& storeId, QString* error) {
    return keep(storeId, Secret::ServerPassword, QByteArray(), error);
}

QString BundledSecrets::encryptionPassword(const QString& storeId, QString* error) {
    const QByteArray bytes = take(storeId, Secret::EncryptionPassword, error);
    if (bytes.isEmpty()) {
        if (error != nullptr && error->isEmpty())
            *error = QStringLiteral("no encryption password for %1 in the secret store")
                         .arg(storeId);
        return QString();
    }
    return QString::fromUtf8(bytes);
}

bool BundledSecrets::setEncryptionPassword(const QString& storeId, const QString& password,
                                           QString* error) {
    return keep(storeId, Secret::EncryptionPassword, password.toUtf8(), error);
}

bool BundledSecrets::clearEncryptionPassword(const QString& storeId, QString* error) {
    return keep(storeId, Secret::EncryptionPassword, QByteArray(), error);
}

}  // namespace zametti
