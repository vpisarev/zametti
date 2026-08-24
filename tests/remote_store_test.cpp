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

using zametti::FolderRemote;
using zametti::RemoteStore;

// Адаптер, НЕ умеющий условной заливки: putIfMatch не переопределён, значит
// работает дефолт RemoteStore — молчаливая деградация в put. Без такого
// адаптера ветка деградации в контракте не проверялась бы вовсе.
class PlainRemote : public zt::MemoryRemote {
public:
    using zt::MemoryRemote::MemoryRemote;
    bool putIfMatch(const QString& name, const QByteArray& bytes,
                    const QString& expectedEtag, QString* etag,
                    bool* preconditionFailed, QString* error = nullptr) override {
        return RemoteStore::putIfMatch(name, bytes, expectedEtag, etag,
                                       preconditionFailed, error);
    }
};

void checkContracts() {
    const QString dir = zt::TestData::outDir(QStringLiteral("remote-folder")) +
                        QStringLiteral("/облако");
    FolderRemote folder(dir);
    zt::checkRemoteContract(folder, "каталог");
    // И ничего не легло рядом с каталогом «сервера»: побег вверх не удался
    // не только по ответу, но и по факту.
    ZT_TRUE("наружу каталога ничего не записано",
            !QFile::exists(QFileInfo(dir).absolutePath() +
                           QStringLiteral("/сбежал.log")));

    zt::MemoryRemote memory;
    zt::checkRemoteContract(memory, "память");

    PlainRemote plain;
    zt::checkRemoteContract(plain, "без If-Match", /*conditionalPuts=*/false);
}

// Рубильники: то, чего у настоящего сервера не спросишь.
void checkFailures() {
    const QString dir = zt::TestData::outDir(QStringLiteral("remote-fail")) +
                        QStringLiteral("/облако");
    FolderRemote remote(dir);
    QString error;
    ZT_TRUE("mkdirOnce", remote.mkdirOnce(&error));

    remote.failNext(QStringLiteral("put"));
    ZT_TRUE("взведённый рубильник валит put",
            !remote.put(QStringLiteral("01aaaa.log"), QByteArray("тело"), nullptr, &error));
    ZT_TRUE("и объясняет", error.contains(QLatin1String("on purpose")));
    ZT_TRUE("рубильник одноразовый: следующий put проходит",
            remote.put(QStringLiteral("01aaaa.log"), QByteArray("тело"), nullptr, &error));

    remote.failNext(QStringLiteral("list"), 2);
    QVector<RemoteStore::Entry> listing;
    ZT_TRUE("первый листинг провален", !remote.list(&listing, &error));
    ZT_TRUE("второй тоже", !remote.list(&listing, &error));
    ZT_TRUE("третий проходит", remote.list(&listing, &error));

    // Обрыв заливки не оставляет на «сервере» полблоба: провалившийся put
    // виден только счётчиком, а содержимое осталось прежним.
    QByteArray body;
    ZT_TRUE("прежнее содержимое цело",
            remote.get(QStringLiteral("01aaaa.log"), &body, nullptr, &error) &&
                body == QByteArray("тело"));
    ZT_TRUE("временных файлов не осталось",
            QDir(dir).entryList(QStringList() << QStringLiteral("*.part"),
                                QDir::Files).isEmpty());
}

// Перевыдача etag без изменения содержимого — болячка WebDAV. Движок обязан
// на этом НЕ заливать заново: он сверяет хеш, а не метку.
void checkEtagReissue() {
    const QString dir = zt::TestData::outDir(QStringLiteral("remote-etag")) +
                        QStringLiteral("/облако");
    FolderRemote remote(dir);
    QString error, before, after;
    ZT_TRUE("mkdirOnce", remote.mkdirOnce(&error));
    ZT_TRUE("put", remote.put(QStringLiteral("01aaaa.log"), QByteArray("тело"),
                              &before, &error));

    remote.setEtagSalt(7);
    QByteArray body;
    ZT_TRUE("get", remote.get(QStringLiteral("01aaaa.log"), &body, &after, &error));
    ZT_TRUE("etag перевыдан другим", after != before);
    ZT_TRUE("а содержимое то же", body == QByteArray("тело"));
}

// Счётчики — под требование брифа мерить синк числами.
void checkCounters() {
    const QString dir = zt::TestData::outDir(QStringLiteral("remote-count")) +
                        QStringLiteral("/облако");
    FolderRemote remote(dir);
    QString error;
    remote.mkdirOnce(&error);
    remote.put(QStringLiteral("01aaaa.log"), QByteArray("тело"), nullptr, &error);
    QVector<RemoteStore::Entry> listing;
    remote.list(&listing, &error);
    QByteArray body;
    remote.get(QStringLiteral("01aaaa.log"), &body, nullptr, &error);
    remote.del(QStringLiteral("01aaaa.log"), &error);

    const FolderRemote::Counters& counters = remote.counters();
    ZT_TRUE("листингов 1", counters.lists == 1);
    ZT_TRUE("заливок 1", counters.puts == 1);
    ZT_TRUE("скачиваний 1", counters.gets == 1);
    ZT_TRUE("удалений 1", counters.dels == 1);
    ZT_TRUE("каталог заводился 1 раз", counters.mkdirs == 1);
    ZT_TRUE("запросов всего 5", remote.traffic().requests == 5);

    remote.resetCounters();
    ZT_TRUE("сброс счётчиков", remote.counters().lists == 0);
}

// Каталога нет — это НЕ «там пусто»: путать их нельзя, пустота значила бы
// «удалить всё локальное».
void checkMissingDir() {
    const QString dir = zt::TestData::outDir(QStringLiteral("remote-nodir")) +
                        QStringLiteral("/которого-нет");
    FolderRemote remote(dir);
    QVector<RemoteStore::Entry> listing;
    QString error;
    ZT_TRUE("листинг несуществующего каталога — отказ", !remote.list(&listing, &error));
    ZT_TRUE("и объяснён", !error.isEmpty());
    ZT_TRUE("mkdirOnce заводит", remote.mkdirOnce(&error));
    ZT_TRUE("теперь листинг удаётся и пуст",
            remote.list(&listing, &error) && listing.isEmpty());
}

}  // namespace

TEST(RemoteStore, All) {
    checkContracts();
    checkFailures();
    checkEtagReissue();
    checkCounters();
    checkMissingDir();
    EXPECT_EQ(0, zt::freshFailures());
}
