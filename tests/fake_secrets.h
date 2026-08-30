// ПОДДЕЛЬНАЯ СВЯЗКА ДЛЯ НАБОРОВ — одна на все проверки.
//
// Живой keyring — вещь машины, а не набора: гонять его в zametti-tests значило
// бы спрашивать человека мастер-пароль на каждом прогоне и гадить в его связку.
// Здесь то же поведение на трёх хэшах.
//
// СЧЁТЧИКИ НЕ УКРАШЕНИЕ. Показ состояния секретов обязан СПРАШИВАТЬ (has), а не
// ЧИТАТЬ: на маке чтение имеет право поднять системный вопрос связки, и окно,
// читающее секрет на всякое переключение строки, начнёт спрашивать пароль само
// — ровно та беда, ради которой keyring и заводился. Набор это доказывает
// числом, а не обещанием: reads обязан остаться нулём.
//
// До 29.08.2026 таких подделок в наборах было три штуки, слово в слово.

#ifndef ZAMETTI_TESTS_FAKE_SECRETS_H
#define ZAMETTI_TESTS_FAKE_SECRETS_H

#include "secret_store.h"

#include <QHash>
#include <QString>

namespace zt {

class FakeSecrets : public zametti::SecretStore {
public:
    using Keyfile = zametti::Keyfile;

    bool available() const override { return true; }

    bool has(const QString& storeId, Secret which) override {
        ++asked;
        switch (which) {
            case Secret::Key: return keys_.contains(storeId);
            case Secret::ServerPassword: return passwords_.contains(storeId);
            case Secret::EncryptionPassword: return cryptPasswords_.contains(storeId);
        }
        return false;
    }

    bool loadKey(const QString& storeId, Keyfile* out, QString* error) override {
        ++reads;
        if (!keys_.contains(storeId)) {
            if (error) *error = QStringLiteral("нет ключа для %1").arg(storeId);
            return false;
        }
        *out = keys_.value(storeId);
        return true;
    }
    bool storeKey(const Keyfile& keyfile, QString*) override {
        keys_.insert(keyfile.storeId(), keyfile);
        return true;
    }
    bool clearKey(const QString& storeId, QString*) override {
        keys_.remove(storeId);
        return true;
    }

    QString serverPassword(const QString& storeId, QString*) override {
        ++reads;
        return passwords_.value(storeId);
    }
    bool setServerPassword(const QString& storeId, const QString& password,
                           QString*) override {
        passwords_.insert(storeId, password);
        return true;
    }
    bool clearServerPassword(const QString& storeId, QString*) override {
        passwords_.remove(storeId);
        return true;
    }

    QString encryptionPassword(const QString& storeId, QString*) override {
        ++reads;
        return cryptPasswords_.value(storeId);
    }
    bool setEncryptionPassword(const QString& storeId, const QString& password,
                               QString*) override {
        cryptPasswords_.insert(storeId, password);
        return true;
    }
    bool clearEncryptionPassword(const QString& storeId, QString*) override {
        cryptPasswords_.remove(storeId);
        return true;
    }

    // Положить ключ, когда набору важно лишь «запись есть». Настоящий, но
    // самый дешёвый: байты ключа наружу не выдаются никому, кроме шифра и
    // воплощений связки, и подделке их взять неоткуда.
    void pretendKey(const QString& storeId) {
        Keyfile made;
        if (Keyfile::create(storeId, QStringLiteral("проба"), {1, quint64(1) << 20},
                            &made, nullptr))
            keys_.insert(storeId, made);
    }

    int asked = 0;   // спросили «есть ли» — дёшево и без вопросов человеку
    int reads = 0;   // ПРОЧИТАЛИ сам секрет

    QHash<QString, Keyfile> keys_;
    QHash<QString, QString> passwords_;
    QHash<QString, QString> cryptPasswords_;
};

}  // namespace zt

#endif  // ZAMETTI_TESTS_FAKE_SECRETS_H
