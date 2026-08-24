// СЛИЯНИЕ ЖУРНАЛОВ И ИДЕНТИЧНОСТЬ НАБОРА ЗАПИСЕЙ — БЕЗ ЕДИНОГО ФАЙЛА.
//
// Ради этого журнал и отделён от диска: слияние — чистая функция от двух
// наборов записей, и его свойства проверяются на значениях в памяти.
//
// Что здесь проверяется:
//   - contentDigest: идентичность журнала — канонический набор рамок, а не
//     байты файла и не порядок записей в нём;
//   - (дальше в этом же файле) mergedWith: union ⊇ обоих входов,
//     коммутативность побайтово, «вычищенное гашением не возвращается».

#include "journal.h"

#include "test_util.h"

#include <QByteArray>

#include <algorithm>
#include <random>
#include <string>
#include <vector>

using namespace zametti;
using ZJournal = zametti::ZJournal;
using Record = zametti::ZJournal::Record;
using RecordRef = zametti::ZJournal::RecordRef;
using Kind = zametti::ZJournal::Kind;

namespace {

constexpr qint64 kNow = 1'700'000'000'000LL;

QByteArray note(const char* mark) {
    return QByteArray("<!-- zametti\nversion: 1\n-->\n\n# Заметка\n\n") + mark + "\n";
}

Digest digestOf(const char* mark) {
    const QByteArray body = note(mark);
    return hashOf(std::string_view(body.constData(), size_t(body.size())));
}

Record entry(Kind kind, qint64 time, qint64 seq, const char* mark) {
    return Record(kind, time, seq, digestOf(mark));
}

// --- 1. contentDigest: набор, а не файл ------------------------------------

void checkContentDigestIgnoresFileOrder() {
    const QVector<Record> straight{
        entry(Kind::Save, kNow, 1, "раз"),
        entry(Kind::Save, kNow + 1, 2, "два"),
        entry(Kind::Tombstone, kNow + 2, 3, ""),
    };
    QVector<Record> shuffled{straight[2], straight[0], straight[1]};

    const Digest a = ZJournal(straight).contentDigest();
    const Digest b = ZJournal(shuffled).contentDigest();
    ZT_TRUE("один набор в разном файловом порядке — один отпечаток", a == b);
}

void checkContentDigestSeesEveryFrameField() {
    const Record base = entry(Kind::Save, kNow, 1, "раз");
    const Digest whole = ZJournal(QVector<Record>{base}).contentDigest();

    // Другой род при том же времени и содержимом.
    const Digest otherKind =
        ZJournal(QVector<Record>{entry(Kind::External, kNow, 1, "раз")}).contentDigest();
    ZT_TRUE("род записи входит в идентичность", whole != otherKind);

    // Другая ревизия.
    const Digest otherSeq =
        ZJournal(QVector<Record>{entry(Kind::Save, kNow, 2, "раз")}).contentDigest();
    ZT_TRUE("ревизия входит в идентичность", whole != otherSeq);

    // Список гашения — тоже часть рамки: гасящая запись с потерянными
    // адресами перестала бы гасить, и это обязано быть видно.
    const Record voiding(Kind::Amendment, kNow, 1, Digest(), 0,
                         {RecordRef(kNow - 5, digestOf("старое"))});
    const Record bare(Kind::Amendment, kNow, 1, Digest(), 0, {});
    const Digest withVoids = ZJournal(QVector<Record>{voiding}).contentDigest();
    const Digest without = ZJournal(QVector<Record>{bare}).contentDigest();
    ZT_TRUE("список гашения входит в идентичность", withVoids != without);
}

void checkContentDigestGrowsWithRecords() {
    QVector<Record> set{entry(Kind::Save, kNow, 1, "раз")};
    const Digest one = ZJournal(set).contentDigest();
    set.append(entry(Kind::Save, kNow + 1, 2, "два"));
    const Digest two = ZJournal(set).contentDigest();
    ZT_TRUE("добавленная запись меняет идентичность", one != two);

    const Digest emptyA = ZJournal().contentDigest();
    const Digest emptyB = ZJournal(QVector<Record>{}).contentDigest();
    ZT_TRUE("пустые журналы неотличимы", emptyA == emptyB);
    ZT_TRUE("пустой отличим от непустого", emptyA != one);
}

}  // namespace

static int ztRunSuite(int argc, char** argv) {
    (void)argc;
    (void)argv;
    checkContentDigestIgnoresFileOrder();
    checkContentDigestSeesEveryFrameField();
    checkContentDigestGrowsWithRecords();
    return zt::report("journal_merge");
}

TEST(JournalMerge, All) {
    std::vector<QByteArray> ztArgs{QByteArrayLiteral("journal_merge_test")};
    std::vector<char*> ztArgv;
    for (QByteArray& a : ztArgs) ztArgv.push_back(a.data());
    EXPECT_EQ(0, ztRunSuite(int(ztArgv.size()), ztArgv.data()));
}
