// EnvSecrets: секреты в переменных среды процесса — наборы и CLI-обвязка.

#include "secret_store.h"

namespace zametti {

bool EnvSecrets::loadKey(const QString& storeId, Keyfile* out, QString* error) {
    Q_ASSERT(out != nullptr);
    if (!qEnvironmentVariableIsSet(kKeyVar)) {
        if (error != nullptr)
            *error = QStringLiteral("%1 is not set").arg(QLatin1String(kKeyVar));
        return false;
    }
    const QByteArray bytes =
        QByteArray::fromBase64(qgetenv(kKeyVar),
                               QByteArray::AbortOnBase64DecodingErrors);
    const Keyfile made = Keyfile::fromLiveKey(storeId, bytes);
    if (!made.hasKey()) {
        if (error != nullptr)
            *error = QStringLiteral("%1 is not base64 of %2 bytes")
                         .arg(QLatin1String(kKeyVar))
                         .arg(Keyfile::kKeyBytes);
        return false;
    }
    *out = made;
    return true;
}

bool EnvSecrets::storeKey(const Keyfile& keyfile, QString* error) {
    if (!keyfile.hasKey()) {
        if (error != nullptr) *error = QStringLiteral("no live key to store");
        return false;
    }
    qputenv(kKeyVar, keyfile.key().toBase64());
    return true;
}

bool EnvSecrets::clearKey(const QString&, QString*) {
    qunsetenv(kKeyVar);
    return true;
}

QString EnvSecrets::serverPassword(const QString&, QString* error) {
    if (!qEnvironmentVariableIsSet(kServerPasswordVar)) {
        if (error != nullptr)
            *error = QStringLiteral("%1 is not set")
                         .arg(QLatin1String(kServerPasswordVar));
        return QString();
    }
    return qEnvironmentVariable(kServerPasswordVar);
}

bool EnvSecrets::setServerPassword(const QString&, const QString& password,
                                   QString*) {
    qputenv(kServerPasswordVar, password.toUtf8());
    return true;
}

bool EnvSecrets::clearServerPassword(const QString&, QString*) {
    qunsetenv(kServerPasswordVar);
    return true;
}

}  // namespace zametti
