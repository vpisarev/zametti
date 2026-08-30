// Свёрток секретов одной записи: правила BundledSecrets и формат SecretBundle.
//
// Живой связки здесь НЕТ (ей не место в наборах — keyring_secrets.h): двери
// платформы подменены памятью со счётчиками. Счётчики не украшение — они
// доказывают числом два правила, на которых стоит политика вопросов владельца
// («максимум один вопрос после пересборки»): свёрток читается один раз за
// прогон, а has() не читает его вовсе.

#include "blob_cipher.h"
#include "bundled_secrets.h"
#include "keyfile.h"
#include "secret_bundle.h"

#include "test_util.h"

#include <QCborMap>
#include <QCborValue>

using namespace zametti;

namespace {

const Keyfile::KdfParams kTiny{1, 1 << 20};

// Связка в памяти: exists/stored/storedIndex — «запись» и её «атрибут»,
// счётчики говорят, кто и сколько раз ходил в «систему».
class MemoryBundle : public BundledSecrets {
public:
    bool available() const override { return true; }

    bool exists = false;
    QByteArray stored;
    QByteArray storedIndex;
    int bundleReads = 0;
    int bundleWrites = 0;
    int indexReads = 0;
    bool failReads = false;   // «система сломалась» — для веток ошибок

protected:
    bool readBundle(QByteArray* data, bool* found, QString* error) override {
        ++bundleReads;
        if (failReads) {
            if (error) *error = QStringLiteral("подделка: чтение запрещено");
            return false;
        }
        *found = exists;
        if (exists) *data = stored;
        return true;
    }
    bool writeBundle(const QByteArray& bundle, const QByteArray& index,
                     QString*) override {
        ++bundleWrites;
        exists = true;   // запись живёт и пустой — её не удаляют (ACL)
        stored = bundle;
        storedIndex = index;
        return true;
    }
    QByteArray readIndex() override {
        ++indexReads;
        return storedIndex;
    }
};

Keyfile makeKey(const QString& storeId) {
    Keyfile made;
    QString error;
    ZT_TRUE("ключ пробы родился",
            Keyfile::create(storeId, QStringLiteral("пароль"), kTiny, &made, &error));
    return made;
}

// Круговорот трёх секретов двух хранилищ в одной записи; соседей не задевает.
void checkRoundTripKeepsNeighbours() {
    MemoryBundle m;
    QString error;
    const Keyfile keyA = makeKey(QStringLiteral("storeA"));
    const Keyfile keyB = makeKey(QStringLiteral("storeB"));
    ZT_TRUE("ключ A лёг", m.storeKey(keyA, &error));
    ZT_TRUE("ключ B лёг", m.storeKey(keyB, &error));
    ZT_TRUE("пароль сервера A лёг",
            m.setServerPassword(QStringLiteral("storeA"), QStringLiteral("пw-A"),
                                &error));
    ZT_TRUE("пароль шифрования A лёг",
            m.setEncryptionPassword(QStringLiteral("storeA"),
                                    QStringLiteral("crypt-A"), &error));

    // Свежий взгляд на ту же «запись»: всё читается тем же.
    MemoryBundle again;
    again.exists = true;
    again.stored = m.stored;
    again.storedIndex = m.storedIndex;
    Keyfile back;
    ZT_TRUE("ключ A читается", again.loadKey(QStringLiteral("storeA"), &back, &error));
    // Байты ключа наружу не выдаются (метод key() закрыт) — сверка чужим
    // судьёй: печать исходным ключом обязана вскрыться прочитанным.
    const BlobAad aad{BlobKind::Journal, QStringLiteral("storeA"),
                      QStringLiteral("probe.log")};
    QByteArray blob, plain;
    ZT_TRUE("исходный ключ запечатал пробу",
            XChaChaCipher::make(keyA, &error)->seal("проба", aad, &blob, &error));
    ZT_TRUE("прочитанный ключ вскрыл печать",
            XChaChaCipher::make(back, &error)->open(blob, aad, &plain, &error) &&
                plain == "проба");
    ZT_EQ("пароль сервера A тот же",
          again.serverPassword(QStringLiteral("storeA"), &error).toStdString(),
          "пw-A");
    ZT_EQ("пароль шифрования A тот же",
          again.encryptionPassword(QStringLiteral("storeA"), &error).toStdString(),
          "crypt-A");

    // Убрать пароль сервера A: сосед B и остальные секреты A на месте.
    ZT_TRUE("пароль сервера A убран",
            again.clearServerPassword(QStringLiteral("storeA"), &error));
    ZT_TRUE("ключ B пережил чужую уборку",
            again.loadKey(QStringLiteral("storeB"), &back, &error));
    ZT_TRUE("ключ A пережил уборку пароля",
            again.loadKey(QStringLiteral("storeA"), &back, &error));
    ZT_TRUE("пароля сервера A больше нет",
            again.serverPassword(QStringLiteral("storeA"), nullptr).isEmpty());
}

// Свёрток читается один раз за прогон — сколько бы секретов ни спросили.
void checkBundleIsReadOnce() {
    MemoryBundle m;
    QString error;
    ZT_TRUE("ключ лёг", m.storeKey(makeKey(QStringLiteral("storeA")), &error));
    m.setServerPassword(QStringLiteral("storeA"), QStringLiteral("пw"), &error);
    Keyfile back;
    m.loadKey(QStringLiteral("storeA"), &back, &error);
    m.serverPassword(QStringLiteral("storeA"), &error);
    m.encryptionPassword(QStringLiteral("storeA"), &error);
    ZT_TRUE("свёрток читался ровно один раз за прогон", m.bundleReads == 1);
}

// has() отвечает индексом и НЕ читает свёрток — та же дисциплина, что
// asked/reads у fake_secrets.h.
void checkHasAsksIndexNotBundle() {
    MemoryBundle writer;
    QString error;
    ZT_TRUE("ключ лёг", writer.storeKey(makeKey(QStringLiteral("storeA")), &error));
    writer.setServerPassword(QStringLiteral("storeA"), QStringLiteral("пw"), &error);

    MemoryBundle reader;
    reader.exists = true;
    reader.stored = writer.stored;
    reader.storedIndex = writer.storedIndex;
    using S = SecretStore::Secret;
    ZT_TRUE("ключ виден по индексу", reader.has(QStringLiteral("storeA"), S::Key));
    ZT_TRUE("пароль сервера виден",
            reader.has(QStringLiteral("storeA"), S::ServerPassword));
    ZT_TRUE("пароля шифрования нет",
            !reader.has(QStringLiteral("storeA"), S::EncryptionPassword));
    ZT_TRUE("чужого хранилища нет", !reader.has(QStringLiteral("storeB"), S::Key));
    ZT_TRUE("свёрток при has() не читался вовсе", reader.bundleReads == 0);
    ZT_TRUE("индекс спрашивался", reader.indexReads >= 4);
}

// Индекс после каждой записи совпадает с содержимым свёртка.
void checkIndexFollowsContent() {
    MemoryBundle m;
    QString error;
    using S = SecretStore::Secret;
    m.storeKey(makeKey(QStringLiteral("storeA")), &error);
    ZT_TRUE("индекс: ключ появился",
            SecretBundle::indexHas(m.storedIndex, QStringLiteral("storeA"), S::Key));
    ZT_TRUE("индекс: пароля нет",
            !SecretBundle::indexHas(m.storedIndex, QStringLiteral("storeA"),
                                    S::ServerPassword));
    m.setServerPassword(QStringLiteral("storeA"), QStringLiteral("пw"), &error);
    ZT_TRUE("индекс: пароль появился",
            SecretBundle::indexHas(m.storedIndex, QStringLiteral("storeA"),
                                   S::ServerPassword));
    m.clearKey(QStringLiteral("storeA"), &error);
    ZT_TRUE("индекс: ключ пропал",
            !SecretBundle::indexHas(m.storedIndex, QStringLiteral("storeA"), S::Key));
}

// Мусор вместо свёртка — отказ словами, не падение; мусорный индекс — «нет».
void checkGarbageIsRefusedInWords() {
    MemoryBundle m;
    m.exists = true;
    m.stored = QByteArrayLiteral("\x00\x01мусор, а не CBOR");
    Keyfile back;
    QString error;
    ZT_TRUE("мусор не читается", !m.loadKey(QStringLiteral("storeA"), &back, &error));
    ZT_TRUE("отказ объяснён словами", !error.isEmpty());
    ZT_TRUE("мусорный индекс — честное «нет»",
            !SecretBundle::indexHas(QByteArrayLiteral("тоже мусор"),
                                    QStringLiteral("storeA"),
                                    SecretStore::Secret::Key));
    // Беда двери — тоже словами.
    MemoryBundle broken;
    broken.failReads = true;
    error.clear();
    ZT_TRUE("беда двери не глотается",
            !broken.loadKey(QStringLiteral("storeA"), &back, &error));
    ZT_TRUE("и тоже объяснена", !error.isEmpty());
}

// Последний clear оставляет ЗАПИСЬ живой (пустой свёрток, не удаление): её
// пересоздание сбрасывало бы ACL и плодило вопросы — политика владельца.
void checkLastClearKeepsTheRecord() {
    MemoryBundle m;
    QString error;
    m.storeKey(makeKey(QStringLiteral("storeA")), &error);
    m.clearKey(QStringLiteral("storeA"), &error);
    ZT_TRUE("запись жива", m.exists);
    SecretBundle empty;
    ZT_TRUE("свёрток разобрался", empty.parse(m.stored));
    ZT_TRUE("и он пуст", empty.isEmpty());
    ZT_TRUE("has() пуст",
            !m.has(QStringLiteral("storeA"), SecretStore::Secret::Key));
}

// Незнакомый слот будущей версии переживает нашу запись: свёрток один на все
// версии программы.
void checkUnknownSlotSurvivesRewrite() {
    // Свёрток «из будущего» собирается руками — чужим судьёй (QCbor), не
    // нашим форматом.
    QCborMap slotMap;
    slotMap.insert(QStringLiteral("future-slot"), QByteArrayLiteral("future"));
    QCborMap stores;
    stores.insert(QStringLiteral("storeA"), slotMap);
    QCborMap root;
    root.insert(QStringLiteral("v"), 1);
    root.insert(QStringLiteral("stores"), stores);

    MemoryBundle m;
    m.exists = true;
    m.stored = QCborValue(root).toCbor();
    QString error;
    ZT_TRUE("свой секрет лёг рядом с незнакомым",
            m.setServerPassword(QStringLiteral("storeA"), QStringLiteral("пw"),
                                &error));
    const QCborMap back = QCborValue::fromCbor(m.stored).toMap();
    const QCborMap backSlots = back.value(QStringLiteral("stores"))
                                   .toMap()
                                   .value(QStringLiteral("storeA"))
                                   .toMap();
    ZT_EQ("незнакомый слот пережил запись",
          backSlots.value(QStringLiteral("future-slot")).toByteArray().toStdString(),
          "future");
    ZT_EQ("и наш лёг",
          backSlots.value(QStringLiteral("webdav")).toByteArray().toStdString(),
          "пw");
}

// Версию выше своей разбор отвергает: переписать её значило бы молча выкинуть
// незнакомое.
void checkNewerVersionIsRefused() {
    QCborMap root;
    root.insert(QStringLiteral("v"), 2);
    root.insert(QStringLiteral("stores"), QCborMap());
    SecretBundle bundle;
    ZT_TRUE("v2 не разбирается", !bundle.parse(QCborValue(root).toCbor()));
}

}  // namespace

TEST(BundledSecrets, All) {
    checkRoundTripKeepsNeighbours();
    checkBundleIsReadOnce();
    checkHasAsksIndexNotBundle();
    checkIndexFollowsContent();
    checkGarbageIsRefusedInWords();
    checkLastClearKeepsTheRecord();
    checkUnknownSlotSurvivesRewrite();
    checkNewerVersionIsRefused();
}
