// ДВИЖОК СИНХРОНИЗАЦИИ — от принятия слитого журнала до полного прогона.
//
// Здесь живут проверки файловой стороны синка: слитый журнал ложится на диск
// только через ZStorage::adoptMergedJournal, и только доказав себя. Ярусы
// обмена, бухгалтерия и материализация добавляются в этот же файл по мере
// появления.

#include "folder_remote.h"
#include "journal.h"
#include "webdav_remote.h"
#include "keyfile.h"
#include "sync_ledger.h"
#include "zstorage.h"

#include "import_limits.h"

#include "mini_store.h"
#include "test_util.h"
#include "testdata.h"
#include "webdav_harness.h"

#include <QThread>

#include <QByteArray>
#include <QFile>
#include <string>
#include <vector>

using namespace zametti;
using ZJournal = zametti::ZJournal;

namespace {

std::string num(qint64 value) { return std::to_string(value); }

constexpr qint64 kNow = 1'700'000'000'000LL;
const char* kId = "01n6cqevzzzzzz";

QByteArray note(const char* mark) {
    return QByteArray("<!-- zametti\nversion: 1\n-->\n\n# Заметка\n\n") + mark + "\n";
}

Digest digestOf(const char* mark) {
    const QByteArray body = note(mark);
    return hashOf(std::string_view(body.constData(), size_t(body.size())));
}

// Журнал заметки, разобранный ЦЕЛИКОМ (Want::All): столько видит слияние.
ZJournal fullJournal(ZStorage& storage, const QString& id) {
    QByteArray bytes;
    QString err;
    ZT_TRUE(("байты журнала прочитались: " + err.toStdString()).c_str(),
            storage.readJournalBytes(id, &bytes, &err));
    ZJournal j;
    if (!bytes.isEmpty())
        ZT_TRUE(("журнал разобрался: " + err.toStdString()).c_str(),
                j.parse(bytes, ZJournal::Want::All, 0, &err));
    return j;
}

// --- 1. adoptMergedJournal: слитое ложится на диск, доказав себя -----------

void checkAdoptMergedJournal() {
    // Два устройства с общим прошлым: одинаковые записи рождаются одинаково
    // (названное время, тот же контент — тот же адрес и та же ревизия).
    zt::MiniStore a, b;
    ZStorage sa(a.root()), sb(b.root());
    QString err;
    for (ZStorage* s : {&sa, &sb}) {
        ZT_TRUE("общая запись легла",
                s->appendToJournal(kId, ZJournal::NewRecord::save(note("раз"),
                                                                 ZJournal::Stamp::at(kNow)), &err));
        ZT_TRUE("общая запись легла",
                s->appendToJournal(kId, ZJournal::NewRecord::save(note("два"),
                                                                 ZJournal::Stamp::at(kNow + 1000)), &err));
    }
    // Расхождение: у A правка, у B — своя правка и надгробие.
    ZT_TRUE("правка A легла",
            sa.appendToJournal(kId, ZJournal::NewRecord::save(note("триА"),
                                                              ZJournal::Stamp::at(kNow + 2000)), &err));
    ZT_TRUE("правка B легла",
            sb.appendToJournal(kId, ZJournal::NewRecord::save(note("триБ"),
                                                              ZJournal::Stamp::at(kNow + 2000)), &err));
    ZT_TRUE("надгробие B легло",
            sb.appendToJournal(kId, ZJournal::NewRecord::tombstone(
                                        ZJournal::Stamp::at(kNow + 3000)), &err));

    const ZJournal ja = fullJournal(sa, kId);
    const ZJournal jb = fullJournal(sb, kId);
    ZJournal merged;
    ZJournal::MergeStats st;
    ZT_TRUE(("слияние прошло: " + err.toStdString()).c_str(),
            ja.mergedWith(jb, &merged, &st, &err));
    ZT_EQ("общих записей две", num(2), num(st.common));

    // CAS-страж: принимающий говорит, какими он видел байты файла журнала.
    QByteArray beforeBytes;
    ZT_TRUE("байты перед принятием прочитались", sa.readJournalBytes(kId, &beforeBytes, &err));
    const Digest seen =
        hashOf(std::string_view(beforeBytes.constData(), size_t(beforeBytes.size())));
    ZT_TRUE(("слитое принято: " + err.toStdString()).c_str(),
            sa.adoptMergedJournal(kId, merged, seen, &err));

    // Файл A теперь несёт union и читается штатным путём.
    const ZJournal after = fullJournal(sa, kId);
    ZT_EQ("записей в файле пять", num(5), num(after.size()));
    ZT_TRUE("набор равен слитому", after.contentDigest() == merged.contentDigest());
    ZT_TRUE("голова — надгробие",
            after.at(after.headIndex()).kind() == ZJournal::Kind::Tombstone);
    // Повторное принятие — с НОВЫМ отпечатком байтов (файл уже слитый).
    QByteArray nowBytes;
    ZT_TRUE("байты слитого прочитались", sa.readJournalBytes(kId, &nowBytes, &err));
    const Digest seenNow =
        hashOf(std::string_view(nowBytes.constData(), size_t(nowBytes.size())));
    ZT_TRUE("повторное принятие прошло", sa.adoptMergedJournal(kId, merged, seenNow, &err));
    ZT_TRUE("набор не изменился",
            fullJournal(sa, kId).contentDigest() == merged.contentDigest());
    // А со СТАРЫМ отпечатком — отказ: файл изменился, слияние устарело.
    ZT_TRUE("устаревший отпечаток отвергнут",
            !sa.adoptMergedJournal(kId, merged, seen, &err));
}

void checkAdoptRefusesUnprovable() {
    zt::MiniStore a;
    ZStorage sa(a.root());
    QString err;
    ZT_TRUE("запись легла",
            sa.appendToJournal(kId, ZJournal::NewRecord::save(note("живое"),
                                                              ZJournal::Stamp::at(kNow)), &err));
    const QByteArray before = [&] {
        QFile f(a.journalOf(kId));
        if (!f.open(QIODevice::ReadOnly)) return QByteArray();
        return f.readAll();
    }();

    // «Слитый» журнал, слепленный из голых рамок: слепков у него нет, значит
    // доказать себя он не может — и файл обязан остаться нетронутым.
    const Digest bogus = hashOf(std::string_view("не то"));
    ZJournal fake(QVector<ZJournal::Record>{
        ZJournal::Record(ZJournal::Kind::Save, kNow + 5000, 7, bogus)});
    const Digest seenBefore =
        hashOf(std::string_view(before.constData(), size_t(before.size())));
    ZT_TRUE("недоказуемое отвергнуто",
            !sa.adoptMergedJournal(kId, fake, seenBefore, &err));
    ZT_TRUE("причина названа", !err.isEmpty());

    QFile f(a.journalOf(kId));
    ZT_TRUE("журнал открылся", f.open(QIODevice::ReadOnly));
    ZT_TRUE("файл не тронут ни байтом", f.readAll() == before);
}

// --- 2. dirty-set: пути записи называют тронутое ---------------------------

void checkDirtySetMarksAndPersists() {
    zt::MiniStore store;
    {
        ZStorage s(store.root());
        QString err;
        ZT_TRUE("чистое хранилище — пустой dirty-set", s.dirtyIds().isEmpty());
        // Запись в журнал — одно из двух горл: пометка ложится сама.
        ZT_TRUE("запись легла",
                s.appendToJournal(kId, ZJournal::NewRecord::save(note("раз"),
                                                                 ZJournal::Stamp::at(kNow)), &err));
        ZT_EQ("тронутая заметка помечена", std::string(kId),
              s.dirtyIds().join(QLatin1Char(',')).toStdString());
        // Повторная правка не плодит дубликатов.
        ZT_TRUE("вторая запись легла",
                s.appendToJournal(kId, ZJournal::NewRecord::save(note("два"),
                                                                 ZJournal::Stamp::at(kNow + 1000)), &err));
        ZT_EQ("пометка одна", num(1), num(s.dirtyIds().size()));
    }
    // Пометки переживают перезапуск: другой экземпляр читает тот же файл.
    {
        ZStorage s(store.root());
        ZT_EQ("пометка пережила перезапуск", num(1), num(s.dirtyIds().size()));
        s.clearDirty({QString::fromLatin1(kId)});
        ZT_TRUE("после чистки пусто", s.dirtyIds().isEmpty());
    }
    {
        ZStorage s(store.root());
        ZT_TRUE("чистота пережила перезапуск", s.dirtyIds().isEmpty());
    }
}

void checkDirtySetClearsOnlyNamed() {
    zt::MiniStore store;
    ZStorage s(store.root());
    s.markDirty(QStringLiteral("01n6cqevaaaaaa"));
    s.markDirty(QStringLiteral("01n6cqevbbbbbb"));
    s.clearDirty({QStringLiteral("01n6cqevaaaaaa"), QStringLiteral("01n6cqevzzzzzz")});
    ZT_EQ("снята только названная", std::string("01n6cqevbbbbbb"),
          s.dirtyIds().join(QLatin1Char(',')).toStdString());
}

// --- 3. SyncLedger: кэш, не истина -----------------------------------------

void checkLedgerRoundTrip() {
    zt::MiniStore store;
    const QString path = store.root() + QStringLiteral("/ledger.json");

    SyncLedger fresh = SyncLedger::load(path);
    ZT_TRUE("отсутствующий файл — пустая бухгалтерия", fresh.isEmpty());
    ZT_TRUE("чистое завершение по умолчанию", fresh.cleanShutdown());

    SyncLedger::Blob b;
    b.etag = QStringLiteral("\"метка-42\"");
    b.sealedHash = hashOf(std::string_view("шифротекст"));
    b.plainHash = hashOf(std::string_view("журнал"));
    fresh.setBlob(QStringLiteral("01n6cqevzzzzzz.log"), b);
    fresh.setFileStat(QStringLiteral("01n6cqevzzzzzz"), {kNow, 137});
    fresh.setCleanShutdown(false);
    QString err;
    ZT_TRUE(("бухгалтерия записалась: " + err.toStdString()).c_str(), fresh.save(&err));

    const SyncLedger back = SyncLedger::load(path);
    const SyncLedger::Blob got = back.blob(QStringLiteral("01n6cqevzzzzzz.log"));
    ZT_EQ("etag пережил дорогу", b.etag.toStdString(), got.etag.toStdString());
    ZT_TRUE("хеш шифротекста пережил дорогу", got.sealedHash == b.sealedHash);
    ZT_TRUE("хеш журнала пережил дорогу", got.plainHash == b.plainHash);
    ZT_EQ("mtime пережил дорогу", num(kNow),
          num(back.fileStat(QStringLiteral("01n6cqevzzzzzz")).mtimeMs));
    ZT_TRUE("нечистое завершение пережило дорогу", !back.cleanShutdown());
    ZT_EQ("known files называет заметку", std::string("01n6cqevzzzzzz"),
          back.knownFiles().join(QLatin1Char(',')).toStdString());
}

void checkLedgerIsACache() {
    zt::MiniStore store;
    const QString path = store.root() + QStringLiteral("/ledger.json");
    // Порча файла — не ошибка, а пустой старт: кэш, не истина (инвариант D).
    {
        QFile f(path);
        ZT_TRUE("файл бухгалтерии записался", f.open(QIODevice::WriteOnly));
        f.write("{ это не json ");
    }
    ZT_TRUE("битый файл — пустая бухгалтерия", SyncLedger::load(path).isEmpty());
    // Незнакомая версия — тоже пустой старт, не отказ.
    {
        QFile f(path);
        ZT_TRUE("файл бухгалтерии записался", f.open(QIODevice::WriteOnly));
        f.write("{\"version\": 99, \"blobs\": {\"x.log\": {\"etag\": \"e\"}}}");
    }
    ZT_TRUE("чужая версия — пустая бухгалтерия", SyncLedger::load(path).isEmpty());
}

void checkLedgerPathsDoNotCollide() {
    // Две копии хранилища с одним storeId на одной машине НЕ делят
    // бухгалтерию: в имени файла — хеш локального пути.
    zt::MiniStore a, b;
    const QString id = QStringLiteral("01n6cqevh7bbfr");
    const QString pa = SyncLedger::pathFor(id, a.root());
    const QString pb = SyncLedger::pathFor(id, b.root());
    ZT_TRUE("пути бухгалтерий различаются", pa != pb);
    ZT_TRUE("имя несёт storeId", pa.contains(id));
    // А один и тот же каталог, названный по-разному, — делит.
    ZT_TRUE("канонизация пути",
            SyncLedger::pathFor(id, a.root() + QStringLiteral("/")) == pa);
}

// --- 4. движок: два устройства и одно облако -------------------------------
//
// Облако — каталог (FolderRemote): счётчики операций отвечают на вопросы
// «сколько стоил прогон», рубильники — на «переживается ли обрыв».
// Второе устройство рождается копией zametti.json (тот же storeId) — ровно
// так живёт настоящая вторая машина.

const Keyfile::KdfParams kTinyKdf{1, 1 << 20};

struct TwoDevices {
    zt::MiniStore a, b, cloudHome;
    QString cloud;
    Keyfile keyfile;
    std::shared_ptr<ZStorage> sa, sb;
    std::shared_ptr<FolderRemote> ra, rb;

    TwoDevices() {
        cloud = cloudHome.root() + QStringLiteral("/облако");
        sa = std::make_shared<ZStorage>(a.root());
        QString err;
        const ZStorage::Identity identity = sa->ensureIdentity(&err);
        ZT_TRUE("идентичность отчеканилась", !identity.isEmpty());
        ZT_TRUE("копия идентичности легла",
                QFile::copy(a.root() + QStringLiteral("/zametti.json"),
                            b.root() + QStringLiteral("/zametti.json")));
        sb = std::make_shared<ZStorage>(b.root());
        ZT_TRUE("ключ отчеканился",
                Keyfile::create(identity.storeId(), QStringLiteral("пароль"), kTinyKdf,
                                &keyfile, &err));
        ra = std::make_shared<FolderRemote>(cloud);
        rb = std::make_shared<FolderRemote>(cloud);
        ZT_TRUE("облако A подключено", sa->setRemote(ra, keyfile, &err));
        ZT_TRUE("облако B подключено", sb->setRemote(rb, keyfile, &err));
    }

    // Правка «vim-ом»: файл пишется мимо программы, находит её stat-скан.
    static void writeRaw(const zt::MiniStore& store, const QString& id, const QByteArray& body) {
        QFile f(store.root() + QStringLiteral("/") + id + QStringLiteral(".md"));
        ZT_TRUE("файл записался", f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(body);
    }
    static QByteArray readRaw(const QString& path) {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) return QByteArray();
        return f.readAll();
    }
    QByteArray noteOf(const zt::MiniStore& store, const QString& id) const {
        return readRaw(store.root() + QStringLiteral("/") + id + QStringLiteral(".md"));
    }
    QByteArray journalOf(const zt::MiniStore& store, const QString& id) const {
        return readRaw(store.journalOf(id));
    }

    ZStorage::SyncReport syncOne(ZStorage& s, const char* what,
                                 ZStorage::SyncOptions options = {}) {
        ZStorage::SyncReport report;
        QString err;
        const bool ok = s.sync(options, &report, &err);
        ZT_TRUE((std::string(what) + ": " + err.toStdString()).c_str(), ok);
        return report;
    }
};

void checkFirstSyncAndSteadyState() {
    TwoDevices rig;
    TwoDevices::writeRaw(rig.a, QStringLiteral("01n6cqevaaaaaa"), note("раз"));
    TwoDevices::writeRaw(rig.a, QStringLiteral("01n6cqevbbbbbb"), note("два"));

    ZStorage::SyncReport first = rig.syncOne(*rig.sa, "первый синк");
    ZT_EQ("обе заметки дожурнализованы", num(2), num(first.baselined));
    ZT_TRUE("журналы уехали",
            QFile::exists(rig.cloud + QStringLiteral("/01n6cqevaaaaaa.log")) &&
                QFile::exists(rig.cloud + QStringLiteral("/01n6cqevbbbbbb.log")));
    ZT_TRUE("манифест уехал", QFile::exists(rig.cloud + QStringLiteral("/zametti.json")));
    ZT_TRUE("ни одного .md в облаке",
            QDir(rig.cloud).entryList({QStringLiteral("*.md")}, QDir::Files).isEmpty());

    // УСТОЙЧИВЫЙ СИНК БЕСПЛАТЕН: один листинг, ноль чтений, ноль заливок.
    rig.ra->resetCounters();
    ZStorage::SyncReport steady = rig.syncOne(*rig.sa, "устойчивый синк");
    ZT_EQ("один листинг", num(1), num(rig.ra->counters().lists));
    ZT_EQ("ноль скачиваний", num(0), num(rig.ra->counters().gets));
    ZT_EQ("ноль заливок", num(0), num(rig.ra->counters().puts));
    ZT_EQ("оба блоба пропущены ярусом 1", num(2), num(steady.skipped));
}

void checkEditTravelsAndMaterializes() {
    TwoDevices rig;
    const QString id = QStringLiteral("01n6cqevaaaaaa");
    TwoDevices::writeRaw(rig.a, id, note("раз"));
    rig.syncOne(*rig.sa, "синк A");
    ZStorage::SyncReport got = rig.syncOne(*rig.sb, "синк B");
    ZT_EQ("журнал принят целиком", num(1), num(got.takenWhole));
    ZT_EQ("файл материализован", num(1), num(got.materialized));
    ZT_TRUE("байты заметки совпали", rig.noteOf(rig.b, id) == rig.noteOf(rig.a, id));
    ZT_TRUE("журналы побайтово равны",
            rig.journalOf(rig.b, id) == rig.journalOf(rig.a, id));

    // Правка «vim-ом» на A: stat-скан находит, external едет, B принимает.
    TwoDevices::writeRaw(rig.a, id, note("раз, поправленный"));
    ZStorage::SyncReport push = rig.syncOne(*rig.sa, "синк A после правки");
    ZT_EQ("правка дожурнализована external-ом", num(1), num(push.externalRecorded));
    ZStorage::SyncReport pull = rig.syncOne(*rig.sb, "синк B после правки");
    ZT_EQ("замечена ровно одна заметка", num(1), num(pull.takenWhole));
    ZT_TRUE("правка доехала байтами",
            rig.noteOf(rig.b, id) == note("раз, поправленный"));
    // Пометки сняты: следующий прогон снова бесплатен.
    rig.rb->resetCounters();
    rig.syncOne(*rig.sb, "устойчивый после правки");
    ZT_EQ("ноль заливок в устойчивом", num(0), num(rig.rb->counters().puts));
}

void checkConcurrentEditsMerge() {
    TwoDevices rig;
    const QString id = QStringLiteral("01n6cqevaaaaaa");
    TwoDevices::writeRaw(rig.a, id, note("общее"));
    rig.syncOne(*rig.sa, "закладка");
    rig.syncOne(*rig.sb, "закладка B");

    // Правки С ОБЕИХ сторон до всякого обмена.
    TwoDevices::writeRaw(rig.a, id, note("правка А"));
    TwoDevices::writeRaw(rig.b, id, note("правка Б"));

    rig.syncOne(*rig.sa, "A заливает свою");
    ZStorage::SyncReport merged = rig.syncOne(*rig.sb, "B сливает");
    ZT_EQ("одно полное слияние", num(1), num(merged.mergedJournals));
    ZStorage::SyncReport aTakes = rig.syncOne(*rig.sa, "A принимает слитое");
    ZT_EQ("A принял целиком", num(1), num(aTakes.takenWhole));

    // Сошлись: журналы побайтово, файлы побайтово, обе правки в истории.
    ZT_TRUE("журналы сошлись побайтово",
            rig.journalOf(rig.a, id) == rig.journalOf(rig.b, id));
    ZT_TRUE("файлы сошлись побайтово", rig.noteOf(rig.a, id) == rig.noteOf(rig.b, id));
    ZJournal all;
    QString err;
    ZT_TRUE("слитый журнал читается",
            all.parse(rig.journalOf(rig.a, id), ZJournal::Want::All, 0, &err));
    bool sawA = false, sawB = false;
    for (int i = 0; i < all.size(); ++i) {
        if (all.at(i).digest() == digestOf("правка А")) sawA = true;
        if (all.at(i).digest() == digestOf("правка Б")) sawB = true;
    }
    ZT_TRUE("правка А не потеряна", sawA);
    ZT_TRUE("правка Б не потеряна", sawB);

    // Устойчиво и бесплатно с обеих сторон.
    rig.ra->resetCounters();
    rig.rb->resetCounters();
    rig.syncOne(*rig.sa, "устойчивый A");
    rig.syncOne(*rig.sb, "устойчивый B");
    ZT_EQ("ноль заливок A", num(0), num(rig.ra->counters().puts));
    ZT_EQ("ноль заливок B", num(0), num(rig.rb->counters().puts));
}

void checkLedgerLossChangesNothing() {
    // ИНВАРИАНТ D: бухгалтерия — кэш. Стёрли — синк дороже (GET), но результат
    // тот же: ни заливок (наборы равны — анти-ping-pong), ни перезаписей.
    TwoDevices rig;
    const QString id = QStringLiteral("01n6cqevaaaaaa");
    TwoDevices::writeRaw(rig.a, id, note("раз"));
    rig.syncOne(*rig.sa, "закладка");

    const QByteArray journalBefore = rig.journalOf(rig.a, id);
    const QByteArray cloudBefore =
        TwoDevices::readRaw(rig.cloud + QStringLiteral("/") + id + QStringLiteral(".log"));
    ZT_TRUE("бухгалтерия стёрта",
            QFile::remove(SyncLedger::pathFor(rig.sa->identity().storeId(), rig.a.root())));

    rig.ra->resetCounters();
    ZStorage::SyncReport redo = rig.syncOne(*rig.sa, "синк без бухгалтерии");
    ZT_EQ("ноль заливок", num(0), num(rig.ra->counters().puts));
    ZT_TRUE("локальный журнал не тронут", rig.journalOf(rig.a, id) == journalBefore);
    ZT_TRUE("облачный блоб не тронут",
            TwoDevices::readRaw(rig.cloud + QStringLiteral("/") + id +
                                QStringLiteral(".log")) == cloudBefore);
    (void)redo;

    // И снова бесплатно: бухгалтерия отстроилась.
    rig.ra->resetCounters();
    rig.syncOne(*rig.sa, "устойчивый после потери");
    ZT_EQ("ноль скачиваний после восстановления", num(0), num(rig.ra->counters().gets));
}

void checkEtagReissueCostsOneGet() {
    // Болячка WebDAV: сервер перевыдал etag, байты те же. Один GET, ноль
    // заливок, дальше снова бесплатно.
    TwoDevices rig;
    const QString id = QStringLiteral("01n6cqevaaaaaa");
    TwoDevices::writeRaw(rig.a, id, note("раз"));
    rig.syncOne(*rig.sa, "закладка");

    rig.ra->setEtagSalt(42);
    rig.ra->resetCounters();
    ZStorage::SyncReport redo = rig.syncOne(*rig.sa, "синк с перевыданными метками");
    ZT_TRUE("перевыдача замечена", redo.etagReissued >= 1);
    ZT_EQ("ноль заливок", num(0), num(rig.ra->counters().puts));
    rig.ra->resetCounters();
    rig.syncOne(*rig.sa, "устойчивый после перевыдачи");
    ZT_EQ("ноль скачиваний", num(0), num(rig.ra->counters().gets));
}

// --- 5. движок: целостность, надгробия, предохранители ---------------------

void checkCorruptLocalIsAbsence() {
    // Битый локальный журнал — absence: принять удалённый целиком, НОЛЬ
    // заливок. Локальный бит-рот никогда не уезжает в облако.
    TwoDevices rig;
    const QString id = QStringLiteral("01n6cqevaaaaaa");
    TwoDevices::writeRaw(rig.a, id, note("раз"));
    rig.syncOne(*rig.sa, "закладка");
    rig.syncOne(*rig.sb, "закладка B");
    const QByteArray healthy = rig.journalOf(rig.b, id);

    // Переворот бита, который «переживает» разбор: запись на месте, сумма
    // рамки не сошлась. Такой и обязан лечиться обменом.
    const QByteArray original = rig.journalOf(rig.a, id);
    bool bent = false;
    for (int off = 0; off < original.size() && !bent; ++off) {
        QByteArray copy = original;
        copy[off] = char(copy[off] ^ 0x01);
        ZJournal probe;
        QString why;
        if (!probe.parse(copy, ZJournal::Want::All, 0, &why)) continue;
        if (probe.damagedCount() == 1 && !probe.tailTrimmed() && probe.size() == 1) {
            QFile f(rig.a.journalOf(id));
            ZT_TRUE("журнал переписался порчей", f.open(QIODevice::WriteOnly));
            f.write(copy);
            bent = true;
        }
    }
    ZT_TRUE("порча изготовилась", bent);
    if (!bent) return;

    rig.ra->resetCounters();
    ZStorage::SyncReport report;
    QString err;
    rig.sa->sync({}, &report, &err);
    ZT_EQ("порча увидена как absence", num(1), num(report.corruptLocalTreatedAsAbsence));
    ZT_EQ("НОЛЬ заливок", num(0), num(rig.ra->counters().puts));
    ZT_TRUE("локальный журнал вылечен облачным", rig.journalOf(rig.a, id) == healthy);
    ZT_TRUE("заметка на месте", rig.noteOf(rig.a, id) == note("раз"));
}

void checkServerLostBlobHealed() {
    // Блоб пропал на сервере при записанном etag — повреждение сервера,
    // лечится перезаливкой.
    TwoDevices rig;
    const QString id = QStringLiteral("01n6cqevaaaaaa");
    TwoDevices::writeRaw(rig.a, id, note("раз"));
    rig.syncOne(*rig.sa, "закладка");
    ZT_TRUE("блоб стёрт с сервера",
            QFile::remove(rig.cloud + QStringLiteral("/") + id + QStringLiteral(".log")));

    ZStorage::SyncReport healed = rig.syncOne(*rig.sa, "лечение");
    ZT_EQ("перезаливка-лечение", num(1), num(healed.healedRemote));
    ZT_TRUE("блоб вернулся",
            QFile::exists(rig.cloud + QStringLiteral("/") + id + QStringLiteral(".log")));
    // И другой девайс его читает.
    ZStorage::SyncReport got = rig.syncOne(*rig.sb, "чтение после лечения");
    ZT_TRUE("заметка доехала", rig.noteOf(rig.b, id) == note("раз"));
    (void)got;
}

void checkTombstoneTravels() {
    TwoDevices rig;
    const QString id = QStringLiteral("01n6cqevaaaaaa");
    TwoDevices::writeRaw(rig.a, id, note("раз"));
    rig.syncOne(*rig.sa, "закладка");
    rig.syncOne(*rig.sb, "закладка B");

    QString err;
    rig.sa->reload();
    ZT_TRUE(("удаление прошло: " + err.toStdString()).c_str(),
            rig.sa->remove(id, ImportLimits(), &err));
    rig.syncOne(*rig.sa, "надгробие уезжает");
    ZStorage::SyncReport got = rig.syncOne(*rig.sb, "надгробие приезжает");
    ZT_EQ("файл убран по надгробию", num(1), num(got.deletesApplied));
    ZT_TRUE("файла на B больше нет",
            !QFile::exists(rig.b.root() + QStringLiteral("/") + id + QStringLiteral(".md")));
    ZT_TRUE("журнал на B остался", QFile::exists(rig.b.journalOf(id)));
}

void checkEditBeatsDelete() {
    // Сценарий владельца: на одной машине удалили, на другой — правили.
    // Правка каузально позже — она и голова; удалённый файл ВОЗВРАЩАЕТСЯ.
    TwoDevices rig;
    const QString id = QStringLiteral("01n6cqevaaaaaa");
    TwoDevices::writeRaw(rig.a, id, note("раз"));
    rig.syncOne(*rig.sa, "закладка");
    rig.syncOne(*rig.sb, "закладка B");

    QString err;
    rig.sa->reload();
    ZT_TRUE("удаление на A прошло", rig.sa->remove(id, ImportLimits(), &err));
    QThread::msleep(5);  // правка позже удаления и по часам
    TwoDevices::writeRaw(rig.b, id, note("правка после удаления"));

    rig.syncOne(*rig.sa, "A заливает надгробие");
    ZStorage::SyncReport merged = rig.syncOne(*rig.sb, "B сливает правку с надгробием");
    ZT_EQ("файл B не тронут", num(0), num(merged.deletesApplied));
    ZT_TRUE("правка жива на B", rig.noteOf(rig.b, id) == note("правка после удаления"));
    ZStorage::SyncReport back = rig.syncOne(*rig.sa, "A принимает слитое");
    ZT_TRUE("файл ВЕРНУЛСЯ на A",
            rig.noteOf(rig.a, id) == note("правка после удаления"));
    ZT_TRUE("журналы сошлись", rig.journalOf(rig.a, id) == rig.journalOf(rig.b, id));
    (void)back;
}

void checkMassDeleteGuardAndDeclareAlive() {
    TwoDevices rig;
    const QStringList ids{QStringLiteral("01n6cqevaaaaaa"), QStringLiteral("01n6cqevbbbbbb"),
                          QStringLiteral("01n6cqevcccccc"), QStringLiteral("01n6cqevdddddd")};
    for (const QString& id : ids) TwoDevices::writeRaw(rig.a, id, note(qPrintable(id)));
    rig.syncOne(*rig.sa, "закладка");
    rig.syncOne(*rig.sb, "закладка B");

    QString err;
    rig.sa->reload();
    for (const QString& id : ids)
        ZT_TRUE("удаление на A прошло", rig.sa->remove(id, ImportLimits(), &err));
    rig.syncOne(*rig.sa, "надгробия уезжают");

    // Порог набора — 3, удалений — 4: предохранитель обязан сработать.
    ZStorage::SyncOptions guard;
    guard.deleteGuard = 3;
    ZStorage::SyncReport held = rig.syncOne(*rig.sb, "предохранитель", guard);
    ZT_EQ("удаления задержаны все", num(4), num(held.pendingDeletes.size()));
    ZT_EQ("не удалено ни одного", num(0), num(held.deletesApplied));
    for (const QString& id : ids)
        ZT_TRUE("файл на месте",
                QFile::exists(rig.b.root() + QStringLiteral("/") + id + QStringLiteral(".md")));

    // ОТКАЗ: заметки объявлены живыми — записи ПОВЕРХ надгробий, облако
    // лечится, и вопрос не повторяется, потому что изменилось состояние.
    ZT_TRUE(("declareAlive прошёл: " + err.toStdString()).c_str(),
            rig.sb->declareAlive(held.pendingDeletes, &err));
    ZStorage::SyncReport heal = rig.syncOne(*rig.sb, "лечение облака", guard);
    ZT_TRUE("вопрос не повторился", heal.pendingDeletes.isEmpty());
    ZStorage::SyncReport restore = rig.syncOne(*rig.sa, "A принимает воскрешение");
    for (const QString& id : ids)
        ZT_TRUE("файл вернулся на A",
                QFile::exists(rig.a.root() + QStringLiteral("/") + id + QStringLiteral(".md")));
    (void)restore;
}

void checkMassDeleteConfirmed() {
    // Вторая фаза: человек подтвердил — повторный прогон с allowMassDelete.
    TwoDevices rig;
    const QStringList ids{QStringLiteral("01n6cqevaaaaaa"), QStringLiteral("01n6cqevbbbbbb"),
                          QStringLiteral("01n6cqevcccccc"), QStringLiteral("01n6cqevdddddd")};
    for (const QString& id : ids) TwoDevices::writeRaw(rig.a, id, note(qPrintable(id)));
    rig.syncOne(*rig.sa, "закладка");
    rig.syncOne(*rig.sb, "закладка B");
    QString err;
    rig.sa->reload();
    for (const QString& id : ids)
        ZT_TRUE("удаление на A прошло", rig.sa->remove(id, ImportLimits(), &err));
    rig.syncOne(*rig.sa, "надгробия уезжают");

    ZStorage::SyncOptions guard;
    guard.deleteGuard = 3;
    ZStorage::SyncReport held = rig.syncOne(*rig.sb, "предохранитель", guard);
    ZT_EQ("удаления задержаны", num(4), num(held.pendingDeletes.size()));
    guard.allowMassDelete = true;
    ZStorage::SyncReport allowed = rig.syncOne(*rig.sb, "подтверждённые удаления", guard);
    ZT_EQ("применены все", num(4), num(allowed.deletesApplied));
    for (const QString& id : ids)
        ZT_TRUE("файла нет",
                !QFile::exists(rig.b.root() + QStringLiteral("/") + id + QStringLiteral(".md")));
}

void checkPushOnlyMode() {
    TwoDevices rig;
    const QString id = QStringLiteral("01n6cqevaaaaaa");
    TwoDevices::writeRaw(rig.a, id, note("общее"));
    rig.syncOne(*rig.sa, "закладка");
    rig.syncOne(*rig.sb, "закладка B");

    // Обе стороны правят одну заметку; A успевает первым полным прогоном.
    TwoDevices::writeRaw(rig.a, id, note("правка А"));
    TwoDevices::writeRaw(rig.b, id, note("правка Б"));
    rig.syncOne(*rig.sa, "A заливает");

    // Выход B: только исходящее, НОЛЬ скачиваний и материализаций; блоб,
    // требующий слияния, отложен без потерь.
    rig.rb->resetCounters();
    ZStorage::SyncOptions exit;
    exit.mode = ZStorage::SyncOptions::PushOnly;
    ZStorage::SyncReport push = rig.syncOne(*rig.sb, "push-only на выходе", exit);
    ZT_EQ("ноль скачиваний", num(0), num(rig.rb->counters().gets));
    ZT_EQ("ноль материализаций", num(0), num(push.materialized));
    ZT_TRUE("слияние отложено", push.deferred >= 1);
    ZT_TRUE("своя правка Б цела", rig.noteOf(rig.b, id) == note("правка Б"));
    // Облако не потеряло правку А.
    ZT_TRUE("блоб А в облаке не перетёрт",
            rig.syncOne(*rig.sb, "полный прогон доделывает").mergedJournals == 1);
    rig.syncOne(*rig.sa, "A принимает слитое");
    ZT_TRUE("сошлись", rig.journalOf(rig.a, id) == rig.journalOf(rig.b, id));

    // Нулевой бюджет: всё отложено, заливок нет, пометки живы.
    TwoDevices::writeRaw(rig.a, id, note("ещё правка"));
    rig.syncOne(*rig.sa, "выравнивание перед бюджетом");
    TwoDevices::writeRaw(rig.a, id, note("и ещё правка"));
    exit.exitPushBudgetSec = 0;
    rig.ra->resetCounters();
    ZStorage::SyncReport broke = rig.syncOne(*rig.sa, "нулевой бюджет", exit);
    ZT_EQ("заливок нет", num(0), num(rig.ra->counters().puts));
    ZT_TRUE("отложено", broke.deferred >= 1);
    ZT_TRUE("пометка пережила бюджет", !rig.sa->dirtyIds().isEmpty());
}

void checkInterruptionHeals() {
    // Инвариант E: обрыв в любой точке безопасен, повторный прогон достраивает.
    TwoDevices rig;
    TwoDevices::writeRaw(rig.a, QStringLiteral("01n6cqevaaaaaa"), note("раз"));
    TwoDevices::writeRaw(rig.a, QStringLiteral("01n6cqevbbbbbb"), note("два"));
    TwoDevices::writeRaw(rig.a, QStringLiteral("01n6cqevcccccc"), note("три"));

    rig.ra->failNext(QStringLiteral("put"), 2);
    ZStorage::SyncReport broken;
    QString err;
    ZT_TRUE("оборванный прогон честно красен", !rig.sa->sync({}, &broken, &err));
    ZT_TRUE("часть работы сделана и посчитана", broken.integrityFailures >= 1);

    ZStorage::SyncReport again = rig.syncOne(*rig.sa, "повторный прогон достраивает");
    for (const char* id : {"01n6cqevaaaaaa", "01n6cqevbbbbbb", "01n6cqevcccccc"})
        ZT_TRUE("блоб долит",
                QFile::exists(rig.cloud + QStringLiteral("/") + QLatin1String(id) +
                              QStringLiteral(".log")));
    rig.ra->resetCounters();
    rig.syncOne(*rig.sa, "устойчивый после лечения");
    ZT_EQ("ноль заливок после достройки", num(0), num(rig.ra->counters().puts));
    (void)again;
}

void checkMobileProfileAndCancel() {
    TwoDevices rig;
    const QString id = QStringLiteral("01n6cqevaaaaaa");
    // Мобильный профиль: скан выключен, работает чистый dirty-set.
    TwoDevices::writeRaw(rig.a, id, note("раз"));
    rig.sa->markDirty(id);  // write-ahead пометка редактора
    ZStorage::SyncOptions mobile;
    mobile.statScan = false;
    ZStorage::SyncReport report = rig.syncOne(*rig.sa, "мобильный профиль", mobile);
    ZT_EQ("обработана ровно помеченная", num(1), num(report.dirtyChecked));
    ZT_TRUE("журнал уехал",
            QFile::exists(rig.cloud + QStringLiteral("/") + id + QStringLiteral(".log")));

    // Отмена повторным кликом: тихо и без потерь.
    auto cancel = std::make_shared<std::atomic<bool>>(true);
    ZStorage::SyncOptions cancelled;
    cancelled.cancel = cancel;
    ZStorage::SyncReport stopped = rig.syncOne(*rig.sa, "отменённый прогон", cancelled);
    ZT_TRUE("отмена замечена", stopped.cancelled);
    // После отмены обычный прогон работает как ни в чём не бывало.
    rig.syncOne(*rig.sa, "после отмены");
}

// --- 6. приёмка на живом WebDAV --------------------------------------------
//
// Тот же движок, что бегал по каталогу, — по настоящему проводу: wsgidav
// поднимается обвязкой на время проверки; нет uvx или сети — ГРОМКИЙ пропуск.

void checkLiveWebDavCycle() {
    zt::WebDavStand stand(zt::TestData::outDir(QStringLiteral("webdav-sync")));
    if (!stand.running()) {
        fprintf(stderr, "sync_engine: WebDAV-стенд не поднялся (%s) — приёмка ПРОПУЩЕНА\n",
                qPrintable(stand.why()));
        return;
    }
    zt::MiniStore a, b;
    ZStorage sa(a.root());
    QString err;
    const ZStorage::Identity identity = sa.ensureIdentity(&err);
    ZT_TRUE("копия идентичности легла",
            QFile::copy(a.root() + QStringLiteral("/zametti.json"),
                        b.root() + QStringLiteral("/zametti.json")));
    ZStorage sb(b.root());
    Keyfile keyfile;
    ZT_TRUE("ключ отчеканился",
            Keyfile::create(identity.storeId(), QStringLiteral("пароль"), kTinyKdf, &keyfile,
                            &err));
    WebDavRemote::Config config;
    config.base = stand.url();
    config.user = QString::fromUtf8(zt::WebDavStand::kUser);
    config.password = QString::fromUtf8(zt::WebDavStand::kPassword);
    config.timeoutMs = 20000;
    auto ra = std::make_shared<WebDavRemote>(config);
    auto rb = std::make_shared<WebDavRemote>(config);
    ZT_TRUE("keyfile уехал на сервер",
            ra->mkdirOnce(&err) &&
                ra->put(QLatin1String(Keyfile::kRemoteName), keyfile.toBytes(), nullptr, &err));
    ZT_TRUE("облако A подключено", sa.setRemote(ra, keyfile, &err));
    ZT_TRUE("облако B подключено", sb.setRemote(rb, keyfile, &err));

    const QString id = QStringLiteral("01n6cqevaaaaaa");
    TwoDevices::writeRaw(a, id, note("живой провод"));
    ZStorage::SyncReport up;
    ZT_TRUE(("синк A по проводу: " + err.toStdString()).c_str(), sa.sync({}, &up, &err));
    ZStorage::SyncReport down;
    ZT_TRUE(("синк B по проводу: " + err.toStdString()).c_str(), sb.sync({}, &down, &err));
    ZT_EQ("заметка материализована", num(1), num(down.materialized));
    ZT_TRUE("байты совпали по проводу",
            TwoDevices::readRaw(b.root() + QStringLiteral("/") + id + QStringLiteral(".md")) ==
                note("живой провод"));

    // Конкурентные правки — через настоящий сервер, с его метками.
    TwoDevices::writeRaw(a, id, note("провод, правка А"));
    TwoDevices::writeRaw(b, id, note("провод, правка Б"));
    ZT_TRUE("A заливает", sa.sync({}, nullptr, &err));
    ZStorage::SyncReport merged;
    ZT_TRUE("B сливает", sb.sync({}, &merged, &err));
    ZT_EQ("одно слияние по проводу", num(1), num(merged.mergedJournals));
    ZT_TRUE("A принимает", sa.sync({}, nullptr, &err));
    QFile fa(a.journalOf(id));
    QFile fb(b.journalOf(id));
    ZT_TRUE("журнал A открылся", fa.open(QIODevice::ReadOnly));
    ZT_TRUE("журнал B открылся", fb.open(QIODevice::ReadOnly));
    ZT_TRUE("журналы сошлись побайтово через живой сервер", fa.readAll() == fb.readAll());
}

// --- 7. NoteTree: полные имена для человека --------------------------------

void checkSubtreeQualifiedNames() {
    zt::MiniStore store;
    ZStorage s(store.root());
    QString err;
    const QString rootId = s.ensureRootNote(&err);
    ZT_TRUE(("корень завёлся: " + err.toStdString()).c_str(), !rootId.isEmpty());

    const auto put = [&](const char* id, const char* title, const QString& parent) {
        QByteArray head("<!-- zametti\nversion: 1\n");
        if (!parent.isEmpty()) head += "parent: " + parent.toUtf8() + "\n";
        head += "-->\n\n# ";
        QFile f(store.root() + QStringLiteral("/") + QLatin1String(id) +
                QStringLiteral(".md"));
        ZT_TRUE("файл записался", f.open(QIODevice::WriteOnly));
        f.write(head + title + "\n");
    };
    put("01n6cqevfffff1", "opencv", QString());
    put("01n6cqevfffff2", "5.1", QStringLiteral("01n6cqevfffff1"));
    put("01n6cqevfffff3", "TODO", QStringLiteral("01n6cqevfffff2"));
    put("01n6cqevfffff4", "сирота", QStringLiteral("01n6cqevnemam1"));  // родителя нет
    s.reload();

    const QString rootTitle = s.info(rootId)->title();
    const ZStorage::NoteTree tree = s.subtreeFor(
        {QStringLiteral("01n6cqevfffff3"), QStringLiteral("01n6cqevfffff4")});
    // Предки подтянулись сами; лишнего не набрано: корень, три ступени, сирота.
    ZT_EQ("в карте ровно пять узлов", num(5), num(tree.size()));
    ZT_EQ("полное имя от корня",
          (rootTitle + QStringLiteral("/opencv/5.1/TODO")).toStdString(),
          tree.qualifiedName(QStringLiteral("01n6cqevfffff3")).toStdString());
    ZT_EQ("папка сама тоже именуема",
          (rootTitle + QStringLiteral("/opencv/5.1")).toStdString(),
          tree.qualifiedName(QStringLiteral("01n6cqevfffff2")).toStdString());
    ZT_EQ("оборванный родитель — честный «?/»", std::string("?/сирота"),
          tree.qualifiedName(QStringLiteral("01n6cqevfffff4")).toStdString());
    ZT_EQ("не из карты — кодовое имя", std::string("01n6cqevcastle"),
          tree.qualifiedName(QStringLiteral("01n6cqevcastle")).toStdString());
    ZT_EQ("корень зовётся своим именем", rootTitle.toStdString(),
          tree.qualifiedName(rootId).toStdString());
}

}  // namespace

static int ztRunSuite(int argc, char** argv) {
    (void)argc;
    (void)argv;
    checkAdoptMergedJournal();
    checkAdoptRefusesUnprovable();
    checkDirtySetMarksAndPersists();
    checkDirtySetClearsOnlyNamed();
    checkLedgerRoundTrip();
    checkLedgerIsACache();
    checkLedgerPathsDoNotCollide();
    checkFirstSyncAndSteadyState();
    checkEditTravelsAndMaterializes();
    checkConcurrentEditsMerge();
    checkLedgerLossChangesNothing();
    checkEtagReissueCostsOneGet();
    checkCorruptLocalIsAbsence();
    checkServerLostBlobHealed();
    checkTombstoneTravels();
    checkEditBeatsDelete();
    checkMassDeleteGuardAndDeclareAlive();
    checkMassDeleteConfirmed();
    checkPushOnlyMode();
    checkInterruptionHeals();
    checkMobileProfileAndCancel();
    checkSubtreeQualifiedNames();
    checkLiveWebDavCycle();
    return zt::report("sync_engine");
}

TEST(SyncEngine, All) {
    std::vector<QByteArray> ztArgs{QByteArrayLiteral("sync_engine_test")};
    std::vector<char*> ztArgv;
    for (QByteArray& a : ztArgs) ztArgv.push_back(a.data());
    EXPECT_EQ(0, ztRunSuite(int(ztArgv.size()), ztArgv.data()));
}
