// Хранилища секретов синхронизации (m17, сессия 3).
//
// Секрета ТРИ, и судьбы у них разные:
//   * МАСТЕР-КЛЮЧ — после разворота keyfile живёт в keyring устройства,
//     чтобы в устойчивом режиме пароль не спрашивался вовсе;
//   * ПАРОЛЬ WebDAV-СЕРВЕРА — тоже keyring; переспрашивается только по 401;
//   * ПАРОЛЬ ШИФРОВАНИЯ — тоже keyring (решение владельца 28.08.2026,
//     отменившее прежнее правило «пароль нигде не живёт»): keychain и есть
//     место, где свой пароль можно подсмотреть и скопировать, — иначе он
//     живёт на бумажке у монитора. Кладётся только ПОСЛЕ удачного разворота
//     или чеканки; для работы синка не нужен (работает ключ) — он для глаз.
//
// Интерфейс — чтобы наборы и CLI ходили через переменные среды (EnvSecrets),
// окно — через системный keyring (KeyringSecrets), а движок не знал разницы.
// Ключ ходит ТОЛЬКО как Keyfile (решение владельца): loadKey отдаёт Keyfile
// с живым ключом (без конверта — конверт живёт на сервере), storeKey кладёт
// байты из Keyfile.

#ifndef ZAMETTI_SYNC_SECRET_STORE_H
#define ZAMETTI_SYNC_SECRET_STORE_H

#include "keyfile.h"

#include <QString>

namespace zametti {

class SecretStore {
public:
    virtual ~SecretStore() = default;

    // Есть ли за интерфейсом живое хранилище (keyring может быть недоступен —
    // редкий WM; тогда деградация решается вызывающим, не здесь).
    virtual bool available() const = 0;

    // Ключ. loadKey: ложь — ключа нет или он негодный, объяснение в error.
    virtual bool loadKey(const QString& storeId, Keyfile* out,
                         QString* error = nullptr) = 0;
    virtual bool storeKey(const Keyfile& keyfile, QString* error = nullptr) = 0;
    virtual bool clearKey(const QString& storeId, QString* error = nullptr) = 0;

    // Пароль сервера. Пусто — не хранится.
    virtual QString serverPassword(const QString& storeId,
                                   QString* error = nullptr) = 0;
    virtual bool setServerPassword(const QString& storeId, const QString& password,
                                   QString* error = nullptr) = 0;
    virtual bool clearServerPassword(const QString& storeId,
                                     QString* error = nullptr) = 0;

    // Пароль шифрования (см. шапку: хранится для глаз, не для синка).
    virtual QString encryptionPassword(const QString& storeId,
                                       QString* error = nullptr) = 0;
    virtual bool setEncryptionPassword(const QString& storeId, const QString& password,
                                       QString* error = nullptr) = 0;
    virtual bool clearEncryptionPassword(const QString& storeId,
                                         QString* error = nullptr) = 0;
};

// Секреты в переменных среды — для наборов и CLI-обвязки (решение владельца;
// бриф: «секреты — через переменные окружения для тестовой обвязки»).
//
//   ZAMETTI_SYNC_KEY        мастер-ключ, base64 от 32 байт
//   ZAMETTI_WEBDAV_PASSWORD пароль сервера
//
// storeId в имена переменных не входит: CLI работает с одним хранилищем за
// запуск. store*/clear* пишут среду СВОЕГО процесса — этого достаточно
// наборам и push-all, наружу ничего не утекает.
class EnvSecrets : public SecretStore {
public:
    static constexpr char kKeyVar[] = "ZAMETTI_SYNC_KEY";
    static constexpr char kServerPasswordVar[] = "ZAMETTI_WEBDAV_PASSWORD";
    // Та же переменная, из которой CLI берёт вводимый пароль: headless-путь
    // хранит его там же, откуда читает.
    static constexpr char kEncryptionPasswordVar[] = "ZAMETTI_SYNC_PASSWORD";

    bool available() const override { return true; }

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

    QString encryptionPassword(const QString& storeId,
                               QString* error = nullptr) override;
    bool setEncryptionPassword(const QString& storeId, const QString& password,
                               QString* error = nullptr) override;
    bool clearEncryptionPassword(const QString& storeId,
                                 QString* error = nullptr) override;
};

}  // namespace zametti

#endif  // ZAMETTI_SYNC_SECRET_STORE_H
