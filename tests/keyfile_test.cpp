// Keyfile: конверт мастер-ключа (m17, сессия 3).
//
// Всё без единого файла на диске — Keyfile чистое значение. Параметры KDF в
// наборе КРОШЕЧНЫЕ (1 проход, 1 МиБ), и это законно по построению: параметры
// живут в самом keyfile, разворот читает их оттуда, а не из констант.
//
// Проверка «create и unwrap дают ОДИН И ТОТ ЖЕ ключ» живёт не здесь, а в
// наборе BlobCipher: у ключа нет публичной двери к байтам (и не будет), так
// что равенство ключей проверяется через настоящего потребителя — печать
// одним экземпляром, вскрытие другим.

#include "keyfile.h"

#include <QJsonDocument>
#include <QJsonObject>

#include "test_util.h"

namespace {

using zametti::Keyfile;

const Keyfile::KdfParams kTiny{1, quint64(1) << 20};
const char kStoreId[] = "01n6cqevh7bbfr";

void checkCreate() {
    Keyfile made;
    QString error;
    ZT_TRUE("create отработал",
            Keyfile::create(kStoreId, "пароль-для-набора", kTiny, &made, &error));
    ZT_TRUE("после create ключ живой", made.hasKey());
    ZT_EQ("storeId уехал в конверт", kStoreId, made.storeId().toStdString());
    ZT_TRUE("created проставлен", !made.created().isEmpty());
    ZT_TRUE("параметры — те, что просили",
            made.params().opslimit == kTiny.opslimit &&
                made.params().memlimitBytes == kTiny.memlimitBytes);

    ZT_TRUE("пустой пароль — отказ",
            !Keyfile::create(kStoreId, "", kTiny, &made, &error));
}

void checkRoundtrip() {
    Keyfile made;
    ZT_TRUE("create", Keyfile::create(kStoreId, "пароль", kTiny, &made, nullptr));
    const QByteArray bytes = made.toBytes();
    ZT_TRUE("конверт непуст", !bytes.isEmpty());

    Keyfile back;
    QString error;
    ZT_TRUE("parse конверта", back.parse(bytes, &error));
    ZT_TRUE("после parse ключ ещё спит", !back.hasKey());
    ZT_TRUE("параметры прочитаны ИЗ ФАЙЛА, не из констант",
            back.params().opslimit == kTiny.opslimit &&
                back.params().memlimitBytes == kTiny.memlimitBytes);

    ZT_TRUE("неверный пароль — отказ", !back.unwrap("не тот пароль", &error));
    ZT_TRUE("отказ объяснён", !error.isEmpty());
    ZT_TRUE("ключ после отказа не живой", !back.hasKey());

    ZT_TRUE("верный пароль будит ключ", back.unwrap("пароль", nullptr));
    ZT_TRUE("ключ живой", back.hasKey());
}

void checkTamper() {
    Keyfile made;
    ZT_TRUE("create", Keyfile::create(kStoreId, "пароль", kTiny, &made, nullptr));
    QJsonObject object = QJsonDocument::fromJson(made.toBytes()).object();

    // Бит в шифротексте ключа — отказ разворота, а не «почти тот» ключ.
    {
        QByteArray wrapped = QByteArray::fromBase64(
            object.value("key").toString().toLatin1());
        wrapped[5] = char(wrapped[5] ^ 0x01);
        QJsonObject spoiled = object;
        spoiled.insert("key", QString::fromLatin1(wrapped.toBase64()));
        Keyfile back;
        ZT_TRUE("порченый конверт разбирается",
                back.parse(QJsonDocument(spoiled).toJson(), nullptr));
        ZT_TRUE("порченый ключ не разворачивается", !back.unwrap("пароль", nullptr));
    }
    // Подкрученный opslimit меняет ключ из пароля — разворот честно падает.
    {
        QJsonObject spoiled = object;
        spoiled.insert("opslimit", 2);
        Keyfile back;
        ZT_TRUE("конверт с чужим opslimit разбирается",
                back.parse(QJsonDocument(spoiled).toJson(), nullptr));
        ZT_TRUE("подкрученный opslimit валит разворот",
                !back.unwrap("пароль", nullptr));
    }
    // Версия новее нашей — вежливый отказ «обнови программу», не порча.
    {
        QJsonObject newer = object;
        newer.insert("version", Keyfile::kVersion + 1);
        Keyfile back;
        QString error;
        ZT_TRUE("новый формат разбирается ради имени",
                back.parse(QJsonDocument(newer).toJson(), nullptr));
        ZT_TRUE("tooNew стоит", back.tooNew());
        ZT_TRUE("будить новый формат нельзя", !back.unwrap("пароль", &error));
        ZT_TRUE("отказ говорит про обновление", error.contains("update"));
    }
}

void checkRewrap() {
    Keyfile made;
    ZT_TRUE("create", Keyfile::create(kStoreId, "старый", kTiny, &made, nullptr));
    const QByteArray before = made.toBytes();

    ZT_TRUE("rewrap на новый пароль",
            made.rewrap("старый", "новый", kTiny, nullptr));
    const QByteArray after = made.toBytes();
    ZT_TRUE("конверт изменился (новая соль и nonce)", before != after);

    Keyfile back;
    ZT_TRUE("parse нового конверта", back.parse(after, nullptr));
    ZT_TRUE("старый пароль больше не подходит", !back.unwrap("старый", nullptr));
    ZT_TRUE("новый пароль разворачивает", back.unwrap("новый", nullptr));

    // Спящий конверт: rewrap сам будит старым паролем.
    Keyfile sleeping;
    ZT_TRUE("parse", sleeping.parse(after, nullptr));
    ZT_TRUE("rewrap спящего с верным старым",
            sleeping.rewrap("новый", "третий", kTiny, nullptr));
    // Отказ проверяется на СВЕЖЕМ спящем: у разбуженного ключ уже живой, и
    // старый пароль ему не нужен (первая редакция проверки была пустышкой).
    Keyfile stillSleeping;
    ZT_TRUE("parse", stillSleeping.parse(after, nullptr));
    ZT_TRUE("rewrap с неверным старым — отказ",
            !stillSleeping.rewrap("не тот", "четвёртый", kTiny, nullptr));
}

void checkForeignKeys() {
    Keyfile made;
    ZT_TRUE("create", Keyfile::create(kStoreId, "пароль", kTiny, &made, nullptr));
    QJsonObject object = QJsonDocument::fromJson(made.toBytes()).object();
    object.insert("futureThing", "прислала старшая сборка");

    Keyfile back;
    ZT_TRUE("parse с чужим ключом", back.parse(QJsonDocument(object).toJson(), nullptr));
    // Сравнение через QStringLiteral: QLatin1String с русским текстом читает
    // байты UTF-8 как латиницу и никогда не совпадает.
    ZT_TRUE("чужой ключ пережил перезапись",
            QJsonDocument::fromJson(back.toBytes())
                .object().value("futureThing").toString() ==
                QStringLiteral("прислала старшая сборка"));
    ZT_TRUE("и конверт по-прежнему разворачивается", back.unwrap("пароль", nullptr));
}

void checkEmpty() {
    Keyfile empty;
    ZT_TRUE("пустой — isEmpty", empty.isEmpty());
    ZT_TRUE("разворачивать нечего", !empty.unwrap("пароль", nullptr));
    ZT_TRUE("собирать нечего", empty.toBytes().isEmpty());
    ZT_TRUE("не-JSON — отказ разбора", !empty.parse("это не json", nullptr));
}

}  // namespace

TEST(Keyfile, All) {
    checkCreate();
    checkRoundtrip();
    checkTamper();
    checkRewrap();
    checkForeignKeys();
    checkEmpty();
    EXPECT_EQ(0, zt::freshFailures());
}
