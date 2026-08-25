// KeyringSecrets — секреты в системном keyring (m17, сессия 3).
//
// Linux: Secret Service API прямо по QDBus (freedesktop-стандарт; GNOME
// Keyring и KWallet оба его реализуют) — НОЛЬ новых зависимостей помимо
// Qt6::DBus. Сессия с алгоритмом plain (решение владельца): шина D-Bus
// сессии локальна и принадлежит пользователю, а кто читает шину — читает и
// память процесса, так что dh-ietf1024 закрывал бы узкий случай ценой
// заметного кода. Метки элементов — из брифа: zametti-key-<storeId> и
// zametti-webdav-<storeId>.
//
// Windows: Credential Manager (CredWriteW/CredReadW/CredDeleteW из Advapi32) —
// тоже ноль новых зависимостей. Имена элементов те же, что у Secret Service:
// zametti-key-<storeId> и zametti-webdav-<storeId>. Подробности — в
// keyring_secrets_wincred.cpp.
//
// mac: заготовка (available() == false) — Security.framework придёт при
// мак-заходе, интерфейс уже его ждёт.
//
// Наборы этот класс НЕ гоняют (живой keyring — вещь машины, не набора):
// приёмка ручная, пробником `zametti-bench keyring`.
//
// В заголовке нет ни одного типа QtDBus — линковка Qt6::DBus остаётся
// PRIVATE у ядра; вся шина живёт в keyring_secrets_dbus.cpp.

#ifndef ZAMETTI_SYNC_KEYRING_SECRETS_H
#define ZAMETTI_SYNC_KEYRING_SECRETS_H

#include "secret_store.h"

#include <memory>

namespace zametti {

class KeyringSecrets : public SecretStore {
public:
    KeyringSecrets();
    ~KeyringSecrets() override;

    // Ложь — нет шины, нет службы секретов или платформа ещё без реализации.
    // Деградацию (спросить пароль, подержать ключ в памяти до выхода) решает
    // вызывающий.
    bool available() const override;

    bool loadKey(const QString& storeId, Keyfile* out,
                 QString* error = nullptr) override;
    bool storeKey(const Keyfile& keyfile, QString* error = nullptr) override;
    bool clearKey(const QString& storeId, QString* error = nullptr) override;

    QString serverPassword(const QString& storeId,
                           QString* error = nullptr) override;
    bool setServerPassword(const QString& storeId, const QString& password,
                           QString* error = nullptr) override;
    bool clearServerPassword(const QString& storeId,
                             QString* error = nullptr) override;

protected:
    // Вся машинерия шины — в .cpp: заголовок чист от QtDBus.
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

}  // namespace zametti

#endif  // ZAMETTI_SYNC_KEYRING_SECRETS_H
