// KeyringSecrets вне Linux — заготовка: available() == false, каждая
// операция честно отвечает «на этой платформе keyring ещё не подключён».
//
// mac получит реализацию на Security.framework (SecItemAdd/CopyMatching,
// плюс галочка iCloud Keychain — kSecAttrSynchronizable, дефолт ВЫКЛ) при
// мак-заходе; Windows (DPAPI) — при Windows-порте. Интерфейс уже их ждёт;
// деградация до тех пор — сессионная: пароль спрашивается при первом синке,
// ключ живёт в памяти процесса до выхода.

#include "keyring_secrets.h"

namespace zametti {
namespace {

bool notHere(QString* error) {
    if (error != nullptr)
        *error = QStringLiteral("the system keyring is not wired up on this "
                                "platform yet");
    return false;
}

}  // namespace

struct KeyringSecrets::Impl {};

KeyringSecrets::KeyringSecrets() : impl_(std::make_shared<Impl>()) {}
KeyringSecrets::~KeyringSecrets() = default;

bool KeyringSecrets::available() const { return false; }

bool KeyringSecrets::loadKey(const QString&, Keyfile*, QString* error) {
    return notHere(error);
}

bool KeyringSecrets::storeKey(const Keyfile&, QString* error) {
    return notHere(error);
}

bool KeyringSecrets::clearKey(const QString&, QString* error) {
    return notHere(error);
}

QString KeyringSecrets::serverPassword(const QString&, QString* error) {
    notHere(error);
    return QString();
}

bool KeyringSecrets::setServerPassword(const QString&, const QString&,
                                       QString* error) {
    return notHere(error);
}

bool KeyringSecrets::clearServerPassword(const QString&, QString* error) {
    return notHere(error);
}

QString KeyringSecrets::encryptionPassword(const QString&, QString* error) {
    notHere(error);
    return QString();
}

bool KeyringSecrets::setEncryptionPassword(const QString&, const QString&,
                                           QString* error) {
    return notHere(error);
}

bool KeyringSecrets::clearEncryptionPassword(const QString&, QString* error) {
    return notHere(error);
}

}  // namespace zametti
