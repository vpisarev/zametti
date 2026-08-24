// ДВИЖОК СИНХРОНИЗАЦИИ — от принятия слитого журнала до полного прогона.
//
// Здесь живут проверки файловой стороны синка: слитый журнал ложится на диск
// только через ZStorage::adoptMergedJournal, и только доказав себя. Ярусы
// обмена, бухгалтерия и материализация добавляются в этот же файл по мере
// появления.

#include "journal.h"
#include "sync_ledger.h"
#include "zstorage.h"

#include "mini_store.h"
#include "test_util.h"

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

    ZT_TRUE(("слитое принято: " + err.toStdString()).c_str(),
            sa.adoptMergedJournal(kId, merged, &err));

    // Файл A теперь несёт union и читается штатным путём.
    const ZJournal after = fullJournal(sa, kId);
    ZT_EQ("записей в файле пять", num(5), num(after.size()));
    ZT_TRUE("набор равен слитому", after.contentDigest() == merged.contentDigest());
    ZT_TRUE("голова — надгробие",
            after.at(after.headIndex()).kind() == ZJournal::Kind::Tombstone);
    // Повторное принятие того же — идемпотентно.
    ZT_TRUE("повторное принятие прошло", sa.adoptMergedJournal(kId, merged, &err));
    ZT_TRUE("набор не изменился",
            fullJournal(sa, kId).contentDigest() == merged.contentDigest());
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
        f.open(QIODevice::ReadOnly);
        return f.readAll();
    }();

    // «Слитый» журнал, слепленный из голых рамок: слепков у него нет, значит
    // доказать себя он не может — и файл обязан остаться нетронутым.
    const Digest bogus = hashOf(std::string_view("не то"));
    ZJournal fake(QVector<ZJournal::Record>{
        ZJournal::Record(ZJournal::Kind::Save, kNow + 5000, 7, bogus)});
    ZT_TRUE("недоказуемое отвергнуто", !sa.adoptMergedJournal(kId, fake, &err));
    ZT_TRUE("причина названа", !err.isEmpty());

    QFile f(a.journalOf(kId));
    f.open(QIODevice::ReadOnly);
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
        f.open(QIODevice::WriteOnly);
        f.write("{ это не json ");
    }
    ZT_TRUE("битый файл — пустая бухгалтерия", SyncLedger::load(path).isEmpty());
    // Незнакомая версия — тоже пустой старт, не отказ.
    {
        QFile f(path);
        f.open(QIODevice::WriteOnly);
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
    return zt::report("sync_engine");
}

TEST(SyncEngine, All) {
    std::vector<QByteArray> ztArgs{QByteArrayLiteral("sync_engine_test")};
    std::vector<char*> ztArgv;
    for (QByteArray& a : ztArgs) ztArgv.push_back(a.data());
    EXPECT_EQ(0, ztRunSuite(int(ztArgv.size()), ztArgv.data()));
}
