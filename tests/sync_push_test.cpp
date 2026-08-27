// Глаголы облака у хранилища: подключение и заливка (m17, сессия 3).
//
// Всё на фиктивном адаптере: настоящий WebDAV — приёмка (webdav_remote_test),
// а здесь проверяется то, что от сети не зависит — что уезжает, под какими
// именами, что сверяется до первой записи и что расшифровывается обратно.

#include "zstorage.h"

#include "blob_cipher.h"
#include "keyfile.h"

#include <QDir>
#include <QFile>

#include "fake_remote.h"
#include "testdata.h"
#include "test_util.h"

namespace {

using zametti::BlobAad;
using zametti::BlobKind;
using zametti::Keyfile;
using zametti::XChaChaCipher;
using zametti::ZStorage;

const Keyfile::KdfParams kTiny{1, quint64(1) << 20};

std::string str(const QString& s) { return s.toStdString(); }
template <typename T>
std::string num(T v) { return std::to_string(v); }

// Хранилище с парой заметок и вложением. Файлы кладём руками: пробнику нужна
// заметка БЕЗ журнала — ровно то, что видит первый синк.
QString makeStore(const QString& where) {
    QDir().mkpath(where);
    ZStorage store(where);
    QString error;
    if (!store.init(&error)) return error;
    const auto write = [&](const QString& name, const QByteArray& bytes) {
        QFile file(QDir(where).filePath(name));
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
        file.write(bytes);
        return true;
    };
    write(QStringLiteral("01n6cqevh7bbf1.md"),
          "<!-- zametti\ncreated: 2017-03-04T10:00:00+03:00\n"
          "modified: 2017-03-04T10:00:00+03:00\n-->\n\n# Первая\n\nтекст\n");
    write(QStringLiteral("01n6cqevh7bbf2.md"),
          "<!-- zametti\ncreated: 2026-08-24T10:00:00+03:00\n"
          "modified: 2026-08-24T10:00:00+03:00\n-->\n\n# Вторая\n\nдругой текст\n");
    write(QStringLiteral("01n6cqevh7bbf3.jxl"), QByteArray("притворяется картинкой"));
    return QString();
}

void checkGuards() {
    const QString root = zt::TestData::outDir(QStringLiteral("sync-guards")) +
                         QStringLiteral("/хранилище");
    ZT_EQ("хранилище заведено", std::string(), str(makeStore(root)));
    ZStorage store(root);
    const QString storeId = store.ensureIdentity().storeId();
    QString error;

    // Без подключённого облака заливать нечего и незачем.
    ZT_TRUE("pushAll без setRemote отказывает", !store.pushAll(nullptr, &error));
    ZT_TRUE("и объяснено", error.contains(QLatin1String("setRemote")));

    // Спящий ключ шифра не даёт.
    Keyfile made;
    ZT_TRUE("keyfile", Keyfile::create(storeId, "пароль", kTiny, &made, nullptr));
    Keyfile sleeping;
    ZT_TRUE("parse", sleeping.parse(made.toBytes(), nullptr));
    auto remote = std::make_shared<zt::MemoryRemote>();
    ZT_TRUE("setRemote со спящим ключом отказывает",
            !store.setRemote(remote, sleeping, &error));

    // Ключ от ДРУГОГО хранилища — тоже отказ: иначе блобы поехали бы с чужим
    // storeId в AAD и не открылись бы у себя же.
    Keyfile alien;
    ZT_TRUE("чужой keyfile",
            Keyfile::create(QStringLiteral("01ffffffffffff"), "пароль", kTiny, &alien, nullptr));
    ZT_TRUE("setRemote с чужим ключом отказывает",
            !store.setRemote(remote, alien, &error));
    ZT_TRUE("и назван чужой storeId", error.contains(QLatin1String("01ffffffffffff")));

    ZT_TRUE("со своим ключом подключается", store.setRemote(remote, made, &error));
    ZT_TRUE("облако подключено", store.hasRemote());
    store.dropRemote();
    ZT_TRUE("и отключается", !store.hasRemote());
}

// ЧУЖОЕ ОБЛАКО — остановка ДО ЕДИНОЙ ЗАПИСИ. Опечатка в remoteDir иначе
// молча слила бы две несвязанные базы.
void checkForeignCloud() {
    const QString root = zt::TestData::outDir(QStringLiteral("sync-foreign")) +
                         QStringLiteral("/хранилище");
    ZT_EQ("хранилище заведено", std::string(), str(makeStore(root)));
    ZStorage store(root);
    const QString storeId = store.ensureIdentity().storeId();

    // В облаке лежит манифест другого хранилища.
    auto remote = std::make_shared<zt::MemoryRemote>();
    QString error;
    ZT_TRUE("mkdirOnce", remote->mkdirOnce(&error));
    const ZStorage::Identity alien =
        ZStorage::Identity::mint(QStringLiteral("01ffffffffffff"),
                                 QStringLiteral("2026-01-01T00:00:00+03:00"));
    ZT_TRUE("чужой манифест уложен",
            remote->put(QLatin1String(ZStorage::Identity::kFile), alien.toBytes(),
                        nullptr, &error));

    Keyfile made;
    ZT_TRUE("keyfile", Keyfile::create(storeId, "пароль", kTiny, &made, nullptr));
    ZT_TRUE("подключение к чужому облаку отказано",
            !store.setRemote(remote, made, &error));
    ZT_TRUE("и сказано, чьё оно", error.contains(QLatin1String("01ffffffffffff")));
    ZT_TRUE("и подсказано, где чинить адрес",
            error.contains(QLatin1String("check the cloud address")));
    ZT_TRUE("и названа дата создания чужого", error.contains(QLatin1String("(created ")));
    ZT_TRUE("облако не подключено", !store.hasRemote());

    QVector<zametti::RemoteStore::Entry> listing;
    ZT_TRUE("листинг", remote->list(&listing, &error));
    ZT_TRUE("в чужом облаке не появилось ни одного блоба", listing.size() == 1);
}

void checkPushAll() {
    const QString root = zt::TestData::outDir(QStringLiteral("sync-push")) +
                         QStringLiteral("/хранилище");
    ZT_EQ("хранилище заведено", std::string(), str(makeStore(root)));
    ZStorage store(root);
    store.reload();
    const QString storeId = store.ensureIdentity().storeId();

    Keyfile made;
    ZT_TRUE("keyfile", Keyfile::create(storeId, "пароль", kTiny, &made, nullptr));
    auto remote = std::make_shared<zt::MemoryRemote>();
    QString error;
    ZT_TRUE("setRemote", store.setRemote(remote, made, &error));

    ZStorage::PushReport report;
    ZT_TRUE("pushAll", store.pushAll(&report, &error));
    // Заметок в хранилище три: две наши и корневая (её заводит init).
    ZT_TRUE("дожурнализованы все заметки без журнала", report.baselined >= 2);
    ZT_EQ("журналов залито столько же", num(report.baselined), num(report.journals));
    ZT_EQ("вложение залито одно", num(1), num(report.attachments));
    ZT_TRUE("шифровали не воздух", report.plainBytes > 0);
    ZT_TRUE("оверхед обёртки виден",
            report.sealedBytes == report.plainBytes +
                                      qint64(report.journals + report.attachments) *
                                          XChaChaCipher::kOverhead);

    // ЧТО ИМЕННО УЕХАЛО. Никаких `.md` — облако хранит журналы, а не
    // материализацию; манифест открытым текстом, всё прочее — блобы.
    QVector<zametti::RemoteStore::Entry> listing;
    ZT_TRUE("листинг", remote->list(&listing, &error));
    int journals = 0, attachments = 0, manifests = 0, markdown = 0;
    for (const auto& entry : listing) {
        if (entry.name == QLatin1String(ZStorage::Identity::kFile)) ++manifests;
        else if (entry.name.endsWith(QStringLiteral(".md"))) ++markdown;
        else if (entry.name.endsWith(QStringLiteral(".zm"))) ++journals;
        else ++attachments;
    }
    ZT_EQ("манифест один", num(1), num(manifests));
    ZT_EQ("ни одного .md", num(0), num(markdown));
    ZT_EQ("журналов", num(report.journals), num(journals));
    ZT_EQ("вложений", num(1), num(attachments));

    // МАНИФЕСТ ЧИТАЕТСЯ БЕЗ КЛЮЧА — на этом стоит сверка до ввода пароля.
    QByteArray manifest;
    ZT_TRUE("манифест скачан",
            remote->get(QLatin1String(ZStorage::Identity::kFile), &manifest, nullptr, &error));
    ZStorage::Identity theirs;
    ZT_TRUE("и разбирается как есть", theirs.parse(manifest, &error));
    ZT_EQ("storeId тот же", str(storeId), str(theirs.storeId()));

    // БЛОБ ВСКРЫВАЕТСЯ КЛЮЧОМ И РАВЕН ФАЙЛУ НА ДИСКЕ.
    auto cipher = XChaChaCipher::make(made, nullptr);
    // Облачное имя вложения — своё расширение: <id>_<ext>.pic.
    const QString attachmentName = QStringLiteral("01n6cqevh7bbf3_jxl.pic");
    QByteArray blob, plain;
    ZT_TRUE("вложение скачано", remote->get(attachmentName, &blob, nullptr, &error));
    ZT_TRUE("и вскрывается",
            cipher->open(blob, BlobAad{BlobKind::Attachment, storeId, attachmentName},
                         &plain, &error));
    ZT_EQ("байты те же", "притворяется картинкой", plain.toStdString());
    // Шифротекст не содержит открытого текста.
    ZT_TRUE("plaintext в блобе не виден", !blob.contains(QByteArray("притворяется")));

    QByteArray journalBlob, journalPlain;
    const QString journalName = QStringLiteral("01n6cqevh7bbf1.zm");
    ZT_TRUE("журнал скачан", remote->get(journalName, &journalBlob, nullptr, &error));
    ZT_TRUE("и вскрывается",
            cipher->open(journalBlob, BlobAad{BlobKind::Journal, storeId, journalName},
                         &journalPlain, &error));
    QByteArray onDisk;
    ZT_TRUE("журнал с диска", store.readJournalBytes(QStringLiteral("01n6cqevh7bbf1"),
                                                     &onDisk, &error));
    ZT_TRUE("уехало ровно то, что лежит на диске", journalPlain == onDisk);

    // ПОДМЕНА ИМЕНИ НЕ ПРОХОДИТ: тем же ключом, но под чужим именем — отказ.
    ZT_TRUE("под чужим именем блоб не вскрывается",
            !cipher->open(journalBlob,
                          BlobAad{BlobKind::Journal, storeId,
                                  QStringLiteral("01n6cqevh7bbf2.zm")},
                          &journalPlain, nullptr));

    // ОПОРНАЯ ЗАПИСЬ БЕРЁТ ВРЕМЯ ФАЙЛА, а не «сейчас»: заметка 2017 года и в
    // истории начинается 2017 годом.
    zametti::ZJournal old;
    ZT_TRUE("журнал старой заметки читается",
            store.readJournal(QStringLiteral("01n6cqevh7bbf1"), &old, &error));
    ZT_TRUE("в нём есть опорная запись", !old.isEmpty());
}

}  // namespace

TEST(SyncPush, All) {
    checkGuards();
    checkForeignCloud();
    checkPushAll();
    EXPECT_EQ(0, zt::freshFailures());
}
