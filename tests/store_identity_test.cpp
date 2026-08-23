// Идентичность хранилища: zametti.json.
//
// Две половины проверяются раздельно, и это нарочно: РАЗБОР — без единого
// файла (класс чистое значение), ФАЙЛ — через хранилище, потому что файлы
// трогает только оно.

#include "store.h"
#include "store_identity.h"
#include "zstorage.h"

#include "test_util.h"
#include "testdata.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include <string>

namespace {

using zametti::ZStorage;
using zametti::store::StoreIdentity;

std::string s(const QString& v) { return v.toStdString(); }

// --- разбор: ни одного файла -------------------------------------------------

void checkParse() {
    StoreIdentity id;
    QString error;
    ZT_TRUE("наш файл разбирается",
            id.parse(R"({"storeId":"01n6cqevh7bbfr","formatVersion":1,
                         "created":"2026-08-23T00:00:00+03:00",
                         "rootNote":"01n6cqevsd7v5e"})", &error));
    ZT_EQ("id прочитан", std::string("01n6cqevh7bbfr"), s(id.storeId()));
    ZT_EQ("корень прочитан", std::string("01n6cqevsd7v5e"), s(id.rootNote()));
    ZT_EQ("версия прочитана", std::string("1"), std::to_string(id.formatVersion()));
    ZT_TRUE("и она не новее нашей", !id.tooNew());

    // Битый JSON, не объект и негодный id — отказ с объяснением, а не молчание.
    for (const char* bad : {"не json вовсе", "[1,2,3]", R"({"storeId":"короткий"})",
                            R"({"formatVersion":1})"}) {
        StoreIdentity broken;
        QString why;
        ZT_TRUE(std::string("отказ на: ") + bad, !broken.parse(bad, &why));
        ZT_TRUE(std::string("и объяснение есть: ") + bad, !why.isEmpty());
    }
}

void checkTooNew() {
    StoreIdentity id;
    ZT_TRUE("будущий формат читается", id.parse(R"({"storeId":"01n6cqevh7bbfr",
                                                   "formatVersion":99})"));
    ZT_TRUE("но объявлен слишком новым", id.tooNew());
}

// Незнакомые ключи не теряются: старшая сборка добавила своё — не нам стирать.
void checkUnknownKeysSurvive() {
    StoreIdentity id;
    ZT_TRUE("разбор прошёл", id.parse(R"({"storeId":"01n6cqevh7bbfr","formatVersion":1,
                                          "cloud":"webdav://example","чужое":42})"));
    const QJsonObject back = QJsonDocument::fromJson(id.toBytes()).object();
    ZT_EQ("чужой ключ вернулся", std::string("webdav://example"),
          s(back.value(QStringLiteral("cloud")).toString()));
    ZT_EQ("и второй тоже", std::string("42"),
          std::to_string(back.value(QStringLiteral("чужое")).toInt()));
    ZT_EQ("id на месте", std::string("01n6cqevh7bbfr"),
          s(back.value(QStringLiteral("storeId")).toString()));
}

// Круг: собрали → разобрали → то же самое.
void checkRoundTrip() {
    StoreIdentity made = StoreIdentity::mint(QStringLiteral("01n6cqevh7bbfr"),
                                             QStringLiteral("2026-08-23T00:00:00+03:00"));
    made.setRootNote(QStringLiteral("01n6cqevsd7v5e"));
    StoreIdentity back;
    ZT_TRUE("свой же файл разбирается", back.parse(made.toBytes()));
    ZT_EQ("id тот же", s(made.storeId()), s(back.storeId()));
    ZT_EQ("время то же", s(made.created()), s(back.created()));
    ZT_EQ("корень тот же", s(made.rootNote()), s(back.rootNote()));
    ZT_EQ("версия та же", std::to_string(made.formatVersion()),
          std::to_string(back.formatVersion()));
}

// --- файл: через хранилище ---------------------------------------------------

void checkMintedOnInit() {
    QTemporaryDir dir;
    ZT_TRUE("временный каталог", dir.isValid());
    const QString root = dir.filePath(QStringLiteral("store"));
    QString error;
    ZT_TRUE("хранилище заведено", zametti::store::initStore(root, &error));
    ZT_TRUE("и файл идентичности появился",
            QFile::exists(root + QStringLiteral("/zametti.json")));

    ZStorage storage(root);
    const StoreIdentity id = storage.identity(&error);
    ZT_TRUE("он читается", !id.isEmpty());
    ZT_TRUE("id годный", id.storeId().size() == 14);
    ZT_TRUE("время заведения проставлено", !id.created().isEmpty());
    ZT_TRUE("корня пока нет", id.rootNote().isEmpty());

    // ЧЕКАНКА ОДИН РАЗ: повторный ensure не меняет id.
    const QString was = id.storeId();
    ZStorage again(root);
    ZT_EQ("id не перечеканивается", s(was), s(again.ensureIdentity(&error).storeId()));
}

void checkEnsureOnOldStore() {
    QTemporaryDir dir;
    const QString root = dir.filePath(QStringLiteral("old"));
    QString error;
    ZT_TRUE("хранилище заведено", zametti::store::initStore(root, &error));
    // Так выглядит хранилище прежней сборки: файла нет.
    ZT_TRUE("файл убран", QFile::remove(root + QStringLiteral("/zametti.json")));

    ZStorage storage(root);
    ZT_TRUE("пустая идентичность до чеканки", storage.identity(&error).isEmpty());
    const StoreIdentity fresh = storage.ensureIdentity(&error);
    ZT_TRUE("чеканка прошла", !fresh.isEmpty());
    ZT_TRUE("файл на месте", QFile::exists(root + QStringLiteral("/zametti.json")));
    ZT_EQ("и читается тем же", s(fresh.storeId()), s(storage.identity().storeId()));
}

void checkRootNoteWritten() {
    QTemporaryDir dir;
    const QString root = dir.filePath(QStringLiteral("store"));
    QString error;
    ZT_TRUE("хранилище заведено", zametti::store::initStore(root, &error));

    ZStorage storage(root);
    ZT_TRUE("корень назван", storage.setRootNote(QStringLiteral("01n6cqevsd7v5e"), &error));
    ZT_EQ("и записан", std::string("01n6cqevsd7v5e"), s(storage.identity().rootNote()));
    // id при этом не тронут.
    ZT_TRUE("id прежний", storage.identity().storeId().size() == 14);
}

// Не хранилище — «нет», а не чеканка в чужом каталоге.
void checkNotAStore() {
    QTemporaryDir dir;
    ZStorage storage(dir.path());
    QString error;
    ZT_TRUE("идентичности нет", storage.identity(&error).isEmpty());
    ZT_TRUE("и сказано почему", !error.isEmpty());
    ZT_TRUE("чеканки не произошло", storage.ensureIdentity().isEmpty());
    ZT_TRUE("и файла не появилось",
            !QFile::exists(dir.filePath(QStringLiteral("zametti.json"))));
}

// Проверка хранилища не считает наш файл чужим.
void checkVerifyAcceptsFile() {
    QTemporaryDir dir;
    const QString root = dir.filePath(QStringLiteral("store"));
    QString error;
    ZT_TRUE("хранилище заведено", zametti::store::initStore(root, &error));
    zametti::store::Report report;
    zametti::store::verifyStore(root, report);
    ZT_EQ("ни одной жалобы", std::string("0"), std::to_string(report.problems));
}

// А формат новее нашего — жалоба.
void checkVerifyRefusesNewer() {
    QTemporaryDir dir;
    const QString root = dir.filePath(QStringLiteral("store"));
    QString error;
    ZT_TRUE("хранилище заведено", zametti::store::initStore(root, &error));
    QFile file(root + QStringLiteral("/zametti.json"));
    ZT_TRUE("файл открылся", file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    file.write(R"({"storeId":"01n6cqevh7bbfr","formatVersion":99})");
    file.close();

    zametti::store::Report report;
    zametti::store::verifyStore(root, report);
    ZT_TRUE("на будущий формат жалуется", report.problems > 0);
}

}  // namespace

TEST(StoreIdentity, All) {
    checkParse();
    checkTooNew();
    checkUnknownKeysSurvive();
    checkRoundTrip();
    checkMintedOnInit();
    checkEnsureOnOldStore();
    checkRootNoteWritten();
    checkNotAStore();
    checkVerifyAcceptsFile();
    checkVerifyRefusesNewer();
    EXPECT_EQ(0, zt::freshFailures());
}
