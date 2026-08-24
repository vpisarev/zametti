// EnvSecrets: секреты в переменных среды (m17, сессия 3).
//
// Ключ ходит только как Keyfile; равенство ключей, как всюду, проверяется
// через потребителя — печать до укладки в «keyring», вскрытие после
// загрузки из него.

#include "blob_cipher.h"
#include "secret_store.h"

#include "test_util.h"

namespace {

using zametti::BlobAad;
using zametti::BlobKind;
using zametti::EnvSecrets;
using zametti::Keyfile;
using zametti::XChaChaCipher;

const char kStoreId[] = "01n6cqevh7bbfr";

void checkKeyRoundtrip() {
    EnvSecrets secrets;
    secrets.clearKey(kStoreId, nullptr);

    Keyfile missing;
    QString error;
    ZT_TRUE("нет переменной — нет ключа",
            !secrets.loadKey(kStoreId, &missing, &error));
    ZT_TRUE("отказ объяснён", error.contains("ZAMETTI_SYNC_KEY"));

    Keyfile made;
    ZT_TRUE("create", Keyfile::create(kStoreId, "пароль",
                                      {1, quint64(1) << 20}, &made, nullptr));
    QByteArray blob;
    ZT_TRUE("печать до укладки",
            XChaChaCipher::make(made, nullptr)
                ->seal("тело", BlobAad{BlobKind::Journal, kStoreId, "x.log"},
                       &blob, nullptr));

    ZT_TRUE("storeKey", secrets.storeKey(made, nullptr));
    Keyfile loaded;
    ZT_TRUE("loadKey", secrets.loadKey(kStoreId, &loaded, nullptr));
    ZT_TRUE("ключ живой", loaded.hasKey());
    ZT_TRUE("конверта у ключа из keyring нет", loaded.toBytes().isEmpty());

    QByteArray plain;
    ZT_TRUE("загруженный ключ вскрывает печать исходного",
            XChaChaCipher::make(loaded, nullptr)
                ->open(blob, BlobAad{BlobKind::Journal, kStoreId, "x.log"},
                       &plain, nullptr));
    ZT_EQ("и читает то же", "тело", plain.toStdString());

    ZT_TRUE("clearKey", secrets.clearKey(kStoreId, nullptr));
    ZT_TRUE("после clear ключа нет", !secrets.loadKey(kStoreId, &loaded, nullptr));

    // Спящий Keyfile укладке не подлежит.
    Keyfile sleeping;
    ZT_TRUE("parse", sleeping.parse(made.toBytes(), nullptr));
    ZT_TRUE("спящий ключ не кладётся", !secrets.storeKey(sleeping, nullptr));

    // Мусор в переменной — честный отказ, не ключ из мусора.
    qputenv(EnvSecrets::kKeyVar, "не base64 вовсе!!");
    ZT_TRUE("мусор в переменной — отказ",
            !secrets.loadKey(kStoreId, &loaded, &error));
    secrets.clearKey(kStoreId, nullptr);
}

void checkServerPassword() {
    EnvSecrets secrets;
    secrets.clearServerPassword(kStoreId, nullptr);

    QString error;
    ZT_TRUE("нет переменной — пусто",
            secrets.serverPassword(kStoreId, &error).isEmpty());
    ZT_TRUE("и объяснено", error.contains("ZAMETTI_WEBDAV_PASSWORD"));

    ZT_TRUE("set", secrets.setServerPassword(kStoreId, "webdav-секрет", nullptr));
    ZT_EQ("прочитан тот же", "webdav-секрет",
          secrets.serverPassword(kStoreId, nullptr).toStdString());
    ZT_TRUE("clear", secrets.clearServerPassword(kStoreId, nullptr));
    ZT_TRUE("после clear пусто", secrets.serverPassword(kStoreId, nullptr).isEmpty());
}

}  // namespace

TEST(Secrets, All) {
    checkKeyRoundtrip();
    checkServerPassword();
    EXPECT_EQ(0, zt::freshFailures());
}
