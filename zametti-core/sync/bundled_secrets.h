// BundledSecrets — правила ОДНОЙ записи связки, общие для всех платформ.
//
// Заказ владельца (30.08.2026): все секреты всех хранилищ лежат в связке
// устройства одной записью-свёртком (формат — secret_bundle.h). Этот класс
// знает ПРАВИЛА обращения с ней; саму связку знают платформенные двери ниже.
// Мак первый (keyring_secrets_keychain.cpp), dbus/wincred переедут на свёрток
// следующими заходами.
//
// Правила (тела — в bundled_secrets.cpp, восстановленном из утраченной
// сессии 29–30.08.2026 дословно):
//
//   * свёрток читается ОДИН РАЗ ЗА ПРОГОН: вопрос системы, если он и будет,
//     случится единожды; пишем свёрток мы сами, поэтому кэш не устаревает;
//   * ЧИТАЕМ ПЕРЕД ЗАПИСЬЮ — иначе затрём чужие секреты в той же записи;
//   * has() отвечает ПО ИНДЕКСУ (readIndex — атрибуты записи, без данных):
//     показ спрашивает это на всякое переключение строки, и читать ради него
//     секреты нельзя (довод — secret_store.h);
//   * убранный секрет — это ЗАПИСЬ ПУСТОГО СЛОТА, а не удаление записи:
//     запись связки никогда не пересоздаётся, пересоздание сбрасывает ACL и
//     плодит системные вопросы (политика владельца: максимум один вопрос
//     после пересборки).

#ifndef ZAMETTI_SYNC_BUNDLED_SECRETS_H
#define ZAMETTI_SYNC_BUNDLED_SECRETS_H

#include "secret_bundle.h"
#include "secret_store.h"

namespace zametti {

class BundledSecrets : public SecretStore {
public:
    bool has(const QString& storeId, Secret which) override;

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

protected:
    // Двери платформы. readBundle: found различает «записи нет» (не беда) и
    // «беда» (ложь + error). writeBundle кладёт данные И индекс одним
    // движением — indeksу нельзя разъехаться со свёртком. readIndex — ТОЛЬКО
    // атрибуты, без данных и без вопросов; нет записи — пустые байты.
    virtual bool readBundle(QByteArray* data, bool* found, QString* error) = 0;
    virtual bool writeBundle(const QByteArray& bundle, const QByteArray& index,
                             QString* error) = 0;
    virtual QByteArray readIndex() = 0;

    // Правила поверх дверей — см. шапку.
    bool load(QString* error);
    QByteArray take(const QString& storeId, Secret which, QString* error);
    bool keep(const QString& storeId, Secret which, const QByteArray& value,
              QString* error);

    SecretBundle bundle_;
    bool loaded_ = false;
};

}  // namespace zametti

#endif  // ZAMETTI_SYNC_BUNDLED_SECRETS_H
