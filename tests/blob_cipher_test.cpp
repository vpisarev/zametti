// Обёртка блоба: печать, вскрытие, отказ на любой подмене (m17, сессия 3).
//
// Здесь же живут две проверки Keyfile, которым нужен потребитель ключа:
// «create и unwrap дают ОДИН И ТОТ ЖЕ ключ» и «rewrap ключа не меняет» — у
// ключа нет публичной двери к байтам, равенство доказывается печатью одним
// экземпляром и вскрытием другим.

#include "blob_cipher.h"

#include <QSet>

#include "test_util.h"

namespace {

using zametti::BlobAad;
using zametti::BlobCipher;
using zametti::BlobKind;
using zametti::Keyfile;
using zametti::XChaChaCipher;

const Keyfile::KdfParams kTiny{1, quint64(1) << 20};
const char kStoreId[] = "01n6cqevh7bbfr";

Keyfile makeKeyfile(const char* password = "пароль") {
    Keyfile made;
    const bool ok = Keyfile::create(kStoreId, password, kTiny, &made, nullptr);
    ZT_TRUE("keyfile для набора создан", ok);
    return made;
}

BlobAad journalAad(const char* name = "01n6cqevsd7v5e.log") {
    return BlobAad{BlobKind::Journal, kStoreId, name};
}

void checkMake() {
    Keyfile sleeping;
    QString error;
    ZT_TRUE("шифр без живого ключа не рождается",
            XChaChaCipher::make(sleeping, &error) == nullptr);
    ZT_TRUE("отказ объяснён", !error.isEmpty());
    ZT_TRUE("с живым ключом рождается",
            XChaChaCipher::make(makeKeyfile(), nullptr) != nullptr);
}

void checkRoundtrip() {
    const Keyfile keyfile = makeKeyfile();
    auto cipher = XChaChaCipher::make(keyfile, nullptr);
    const QByteArray plain("содержимое журнала, много байт подряд");

    QByteArray blob;
    ZT_TRUE("печать", cipher->seal(plain, journalAad(), &blob, nullptr));
    ZT_TRUE("оверхед обёртки — ровно константа",
            blob.size() == plain.size() + XChaChaCipher::kOverhead);

    QByteArray back;
    ZT_TRUE("вскрытие", cipher->open(blob, journalAad(), &back, nullptr));
    ZT_TRUE("вскрытое равно исходнику", back == plain);

    // Пустой plaintext — законный блоб.
    QByteArray emptyBlob, emptyBack;
    ZT_TRUE("печать пустого", cipher->seal(QByteArray(), journalAad(), &emptyBlob, nullptr));
    ZT_TRUE("вскрытие пустого", cipher->open(emptyBlob, journalAad(), &emptyBack, nullptr));
    ZT_TRUE("пустой и вернулся пустым", emptyBack.isEmpty());
}

// Ключ, развёрнутый из конверта, — ТОТ ЖЕ, что родился в create; rewrap его
// не меняет. Печатает один экземпляр Keyfile, вскрывает другой.
void checkSameKeyAcrossEnvelope() {
    Keyfile made = makeKeyfile("старый");
    auto sealer = XChaChaCipher::make(made, nullptr);
    QByteArray blob;
    ZT_TRUE("печать создателем",
            sealer->seal("секретное тело", journalAad(), &blob, nullptr));

    Keyfile back;
    ZT_TRUE("parse конверта", back.parse(made.toBytes(), nullptr));
    ZT_TRUE("unwrap", back.unwrap("старый", nullptr));
    QByteArray plain;
    ZT_TRUE("развёрнутый ключ вскрывает печать создателя",
            XChaChaCipher::make(back, nullptr)->open(blob, journalAad(), &plain, nullptr));
    ZT_EQ("и читает то же", "секретное тело", plain.toStdString());

    ZT_TRUE("rewrap", back.rewrap("старый", "новый", kTiny, nullptr));
    Keyfile rewrapped;
    ZT_TRUE("parse после rewrap", rewrapped.parse(back.toBytes(), nullptr));
    ZT_TRUE("unwrap новым паролем", rewrapped.unwrap("новый", nullptr));
    plain.clear();
    ZT_TRUE("ключ после смены пароля ТОТ ЖЕ",
            XChaChaCipher::make(rewrapped, nullptr)->open(blob, journalAad(), &plain, nullptr));
    ZT_EQ("и текст тот же", "секретное тело", plain.toStdString());
}

void checkTamper() {
    const Keyfile keyfile = makeKeyfile();
    auto cipher = XChaChaCipher::make(keyfile, nullptr);
    const QByteArray plain("что-то достаточно длинное, чтобы было что портить");
    QByteArray blob;
    ZT_TRUE("печать", cipher->seal(plain, journalAad(), &blob, nullptr));

    // КАЖДЫЙ байт заголовка: магия, версия, nonce — порча любого = отказ.
    const int header = XChaChaCipher::kOverhead - 16;
    for (int i = 0; i < header; ++i) {
        QByteArray spoiled = blob;
        spoiled[i] = char(spoiled[i] ^ 0x01);
        QByteArray sink;
        if (cipher->open(spoiled, journalAad(), &sink, nullptr)) {
            ZT_TRUE(("байт заголовка №" + std::to_string(i) +
                     " перевёрнут, а блоб прошёл").c_str(), false);
        }
    }
    // Выборочные байты тела и тега.
    for (int i : {header, header + 5, int(blob.size()) - 17, int(blob.size()) - 1}) {
        QByteArray spoiled = blob;
        spoiled[i] = char(spoiled[i] ^ 0x40);
        QByteArray sink;
        if (cipher->open(spoiled, journalAad(), &sink, nullptr)) {
            ZT_TRUE(("байт тела №" + std::to_string(i) +
                     " перевёрнут, а блоб прошёл").c_str(), false);
        }
    }
    ZT_TRUE("порча заголовка и тела ловится", true);

    // Подмена любой части AAD — отказ: не тот тип, не то имя, не то хранилище.
    QByteArray sink;
    ZT_TRUE("блоб журнала не вскрывается как вложение",
            !cipher->open(blob, BlobAad{BlobKind::Attachment, kStoreId,
                                        "01n6cqevsd7v5e.log"}, &sink, nullptr));
    ZT_TRUE("под чужим именем не вскрывается",
            !cipher->open(blob, journalAad("01другойid.log"), &sink, nullptr));
    ZT_TRUE("в чужом хранилище не вскрывается",
            !cipher->open(blob, BlobAad{BlobKind::Journal, "01ff0000000000",
                                        "01n6cqevsd7v5e.log"}, &sink, nullptr));

    // Чужой ключ — отказ.
    ZT_TRUE("чужим ключом не вскрывается",
            !XChaChaCipher::make(makeKeyfile("другой"), nullptr)
                 ->open(blob, journalAad(), &sink, nullptr));

    // Версия из будущего — вежливый отказ до всякого крипто.
    QByteArray newer = blob;
    newer[4] = char(XChaChaCipher::kWrapVersion + 1);
    QString error;
    ZT_TRUE("обёртка новее сборки — отказ",
            !cipher->open(newer, journalAad(), &sink, &error));
    ZT_TRUE("отказ говорит про обновление", error.contains("update"));

    // Огрызок и чужие байты.
    ZT_TRUE("огрызок — отказ", !cipher->open(blob.left(20), journalAad(), &sink, nullptr));
    ZT_TRUE("не-блоб — отказ",
            !cipher->open(QByteArray("просто текст, не блоб"), journalAad(), &sink, nullptr));
}

// Инвариант F брифа: nonce не повторяется; plaintext в шифротексте не виден.
void checkNoncesAndLeaks() {
    const Keyfile keyfile = makeKeyfile();
    auto cipher = XChaChaCipher::make(keyfile, nullptr);
    const QByteArray plain =
        QByteArray("шестнадцать байт").repeated(8);   // приметный узор

    QSet<QByteArray> nonces;
    QSet<QByteArray> blobs;
    const int rounds = 300;
    for (int i = 0; i < rounds; ++i) {
        QByteArray blob;
        ZT_TRUE("печать", cipher->seal(plain, journalAad(), &blob, nullptr));
        nonces.insert(blob.mid(5, 24));
        blobs.insert(blob);
        // Ни одно 16-байтовое окно plaintext не всплывает в блобе.
        if (i == 0) {
            bool leaked = false;
            for (int at = 0; at + 16 <= plain.size(); at += 8)
                leaked = leaked || blob.contains(plain.mid(at, 16));
            ZT_TRUE("подстрок plaintext в шифротексте нет", !leaked);
        }
    }
    ZT_TRUE("nonce не повторился ни разу", nonces.size() == rounds);
    ZT_TRUE("одинаковое содержимое — разные байты блоба", blobs.size() == rounds);
}

}  // namespace

TEST(BlobCipher, All) {
    checkMake();
    checkRoundtrip();
    checkSameKeyAcrossEnvelope();
    checkTamper();
    checkNoncesAndLeaks();
    EXPECT_EQ(0, zt::freshFailures());
}
