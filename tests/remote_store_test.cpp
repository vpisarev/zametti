// Адаптер удалённого хранилища: контракт и рубильники (m17, сессия 3).
//
// Контракт один на всех (fake_remote.h) и гоняется по ДВУМ реализациям:
// каталог на диске и память. Так проверяется обещание брифа — каркас должен
// допускать S3 без правки движка, а S3 не каталог.

#include "fake_remote.h"
#include "folder_remote.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

#include "testdata.h"
#include "test_util.h"

namespace {

using zametti::FolderCloud;
using zametti::CloudStore;

// Адаптер, НЕ умеющий условной заливки: putIfMatch не переопределён, значит
// работает дефолт CloudStore — молчаливая деградация в put. Без такого
// адаптера ветка деградации в контракте не проверялась бы вовсе.
class PlainCloud : public zt::MemoryCloud {
public:
    using zt::MemoryCloud::MemoryCloud;
    bool putIfMatch(const QString& name, const QByteArray& bytes,
                    const QString& expectedEtag, QString* etag,
                    bool* preconditionFailed, QString* error = nullptr) override {
        return CloudStore::putIfMatch(name, bytes, expectedEtag, etag,
                                       preconditionFailed, error);
    }
};

void checkContracts() {
    const QString dir = zt::TestData::outDir(QStringLiteral("cloud-folder")) +
                        QStringLiteral("/облако");
    FolderCloud folder(dir);
    zt::checkCloudContract(folder, "каталог");
    // И ничего не легло рядом с каталогом «сервера»: побег вверх не удался
    // не только по ответу, но и по факту.
    ZT_TRUE("наружу каталога ничего не записано",
            !QFile::exists(QFileInfo(dir).absolutePath() +
                           QStringLiteral("/сбежал.log")));

    zt::MemoryCloud memory;
    zt::checkCloudContract(memory, "память");

    PlainCloud plain;
    zt::checkCloudContract(plain, "без If-Match", /*conditionalPuts=*/false);
}

// Рубильники: то, чего у настоящего сервера не спросишь.
void checkFailures() {
    const QString dir = zt::TestData::outDir(QStringLiteral("cloud-fail")) +
                        QStringLiteral("/облако");
    FolderCloud cloud(dir);
    QString error;
    ZT_TRUE("mkdirOnce", cloud.mkdirOnce(&error));

    cloud.failNext(QStringLiteral("put"));
    ZT_TRUE("взведённый рубильник валит put",
            !cloud.put(QStringLiteral("01aaaa.log"), QByteArray("тело"), nullptr, &error));
    ZT_TRUE("и объясняет", error.contains(QLatin1String("on purpose")));
    ZT_TRUE("рубильник одноразовый: следующий put проходит",
            cloud.put(QStringLiteral("01aaaa.log"), QByteArray("тело"), nullptr, &error));

    cloud.failNext(QStringLiteral("list"), 2);
    QVector<CloudStore::Entry> listing;
    ZT_TRUE("первый листинг провален", !cloud.list(&listing, &error));
    ZT_TRUE("второй тоже", !cloud.list(&listing, &error));
    ZT_TRUE("третий проходит", cloud.list(&listing, &error));

    // Обрыв заливки не оставляет на «сервере» полблоба: провалившийся put
    // виден только счётчиком, а содержимое осталось прежним.
    QByteArray body;
    ZT_TRUE("прежнее содержимое цело",
            cloud.get(QStringLiteral("01aaaa.log"), &body, nullptr, &error) &&
                body == QByteArray("тело"));
    ZT_TRUE("временных файлов не осталось",
            QDir(dir).entryList(QStringList() << QStringLiteral("*.part"),
                                QDir::Files).isEmpty());
}

// Перевыдача etag без изменения содержимого — болячка WebDAV. Движок обязан
// на этом НЕ заливать заново: он сверяет хеш, а не метку.
void checkEtagReissue() {
    const QString dir = zt::TestData::outDir(QStringLiteral("cloud-etag")) +
                        QStringLiteral("/облако");
    FolderCloud cloud(dir);
    QString error, before, after;
    ZT_TRUE("mkdirOnce", cloud.mkdirOnce(&error));
    ZT_TRUE("put", cloud.put(QStringLiteral("01aaaa.log"), QByteArray("тело"),
                              &before, &error));

    cloud.setEtagSalt(7);
    QByteArray body;
    ZT_TRUE("get", cloud.get(QStringLiteral("01aaaa.log"), &body, &after, &error));
    ZT_TRUE("etag перевыдан другим", after != before);
    ZT_TRUE("а содержимое то же", body == QByteArray("тело"));
}

// Счётчики — под требование брифа мерить синк числами.
void checkCounters() {
    const QString dir = zt::TestData::outDir(QStringLiteral("cloud-count")) +
                        QStringLiteral("/облако");
    FolderCloud cloud(dir);
    QString error;
    cloud.mkdirOnce(&error);
    cloud.put(QStringLiteral("01aaaa.log"), QByteArray("тело"), nullptr, &error);
    QVector<CloudStore::Entry> listing;
    cloud.list(&listing, &error);
    QByteArray body;
    cloud.get(QStringLiteral("01aaaa.log"), &body, nullptr, &error);
    cloud.del(QStringLiteral("01aaaa.log"), &error);

    const FolderCloud::Counters& counters = cloud.counters();
    ZT_TRUE("листингов 1", counters.lists == 1);
    ZT_TRUE("заливок 1", counters.puts == 1);
    ZT_TRUE("скачиваний 1", counters.gets == 1);
    ZT_TRUE("удалений 1", counters.dels == 1);
    ZT_TRUE("каталог заводился 1 раз", counters.mkdirs == 1);
    ZT_TRUE("запросов всего 5", cloud.traffic().requests == 5);

    cloud.resetCounters();
    ZT_TRUE("сброс счётчиков", cloud.counters().lists == 0);
}

// Каталога нет — это НЕ «там пусто»: путать их нельзя, пустота значила бы
// «удалить всё локальное».
void checkMissingDir() {
    const QString dir = zt::TestData::outDir(QStringLiteral("cloud-nodir")) +
                        QStringLiteral("/которого-нет");
    FolderCloud cloud(dir);
    QVector<CloudStore::Entry> listing;
    QString error;
    ZT_TRUE("листинг несуществующего каталога — отказ", !cloud.list(&listing, &error));
    ZT_TRUE("и объяснён", !error.isEmpty());
    ZT_TRUE("mkdirOnce заводит", cloud.mkdirOnce(&error));
    ZT_TRUE("теперь листинг удаётся и пуст",
            cloud.list(&listing, &error) && listing.isEmpty());
}

}  // namespace

TEST(CloudStore, All) {
    checkContracts();
    checkFailures();
    checkEtagReissue();
    checkCounters();
    checkMissingDir();
    EXPECT_EQ(0, zt::freshFailures());
}
