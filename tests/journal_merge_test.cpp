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

std::string num(qint64 value) { return std::to_string(value); }

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

// --- обвязка: журнал из настоящих байтов, но без единого файла --------------
//
// Слиянию нужны слепки, а их укладку (поколения, дельты, суммы рамок) знает
// только сам журнал. Поэтому журнал тут не лепится из голых Record, а
// СТРОИТСЯ так же, как строит его хранилище: headerBytes + composeRecord по
// одной записи, только копятся байты в памяти, а не в файле.
struct Builder {
    QByteArray bytes;

    explicit Builder(const QString& clean = QString::fromLatin1(ZJournal::kCleanVersion)) {
        bytes = ZJournal::headerBytes(clean);
    }

    ZJournal parsed() const {
        ZJournal j;
        QString err;
        ZT_TRUE(("журнал разобрался: " + err.toStdString()).c_str(),
                j.parse(bytes, ZJournal::Want::All, 0, &err));
        return j;
    }

    void add(const ZJournal::NewRecord& what) {
        ZJournal j;
        QString err;
        if (!j.parse(bytes, ZJournal::Want::All, 0, &err)) {
            ZT_TRUE("байты для дозаписи разобрались", false);
            return;
        }
        ZJournal::Record frame;
        QByteArray rec;
        const bool ok = j.composeRecord(what, 0, &frame, &rec, &err);
        ZT_TRUE(("запись собралась: " + err.toStdString()).c_str(), ok);
        if (ok) bytes += rec;
    }
};

QByteArray serialized(const ZJournal& j) {
    QVector<int> all;
    for (int i = 0; i < j.size(); ++i) all.append(i);
    QByteArray out;
    QString err;
    ZT_TRUE(("журнал сериализовался: " + err.toStdString()).c_str(),
            j.toBytes(all, j.cleanVersion(), &out, &err));
    return out;
}

// Независимая от встроенной самопроверки ревизия: каждая запись входа либо
// присутствует в слитом своим адресом, либо погашена его записью.
bool accountedFor(const ZJournal& merged, const Record& r) {
    for (int i = 0; i < merged.size(); ++i) {
        const Record& e = merged.at(i);
        if (e.time() == r.time() && e.digest() == r.digest()) return true;
        if (e.voidsRecord(r)) return true;
    }
    return false;
}

void checkSuperset(const char* what, const ZJournal& merged, const ZJournal& in) {
    for (int i = 0; i < in.size(); ++i)
        ZT_TRUE(what, accountedFor(merged, in.at(i)));
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

// --- 2. mergedWith: union, коммутативность, гашение ------------------------

void checkMergeUnionAndCommutes() {
    Builder common;
    common.add(ZJournal::NewRecord::save(note("раз"), ZJournal::Stamp::at(kNow)));
    common.add(ZJournal::NewRecord::save(note("два"), ZJournal::Stamp::at(kNow + 1000)));

    Builder a = common;
    a.add(ZJournal::NewRecord::external(note("триА"), ZJournal::Stamp::at(kNow + 2000)));

    Builder b = common;
    b.add(ZJournal::NewRecord::save(note("триБ"), ZJournal::Stamp::at(kNow + 2000)));
    b.add(ZJournal::NewRecord::tombstone(ZJournal::Stamp::at(kNow + 3000)));

    const ZJournal ja = a.parsed();
    const ZJournal jb = b.parsed();

    ZJournal ab, ba;
    ZJournal::MergeStats st;
    QString err;
    ZT_TRUE(("A+B слились: " + err.toStdString()).c_str(), ja.mergedWith(jb, &ab, &st, &err));
    ZT_TRUE(("B+A слились: " + err.toStdString()).c_str(), jb.mergedWith(ja, &ba, nullptr, &err));

    checkSuperset("слитое покрывает A", ab, ja);
    checkSuperset("слитое покрывает B", ab, jb);
    ZT_EQ("общих записей две", num(2), num(st.common));
    ZT_EQ("уникальных у A одна", num(1), num(st.fromThis));
    ZT_EQ("уникальных у B две", num(2), num(st.fromOther));
    ZT_EQ("записей в union пять", num(5), num(ab.size()));

    ZT_TRUE("merge(A,B) == merge(B,A) побайтово", serialized(ab) == serialized(ba));
    ZT_TRUE("идентичность слитых равна", ab.contentDigest() == ba.contentDigest());

    // Голова union — надгробие с наибольшей ревизией; головы входов не потеряны.
    ZT_TRUE("голова слитого есть", ab.headIndex() >= 0);
    ZT_TRUE("голова слитого — надгробие",
            ab.at(ab.headIndex()).kind() == ZJournal::Kind::Tombstone);

    // Сериализованное читается обратно в тот же набор.
    ZJournal re;
    ZT_TRUE("слитое разобралось обратно", re.parse(serialized(ab), ZJournal::Want::All, 0, &err));
    ZT_TRUE("набор пережил укладку", re.contentDigest() == ab.contentDigest());

    // Поглощение: merge(слитое, вход) ничего не меняет.
    ZJournal again;
    ZT_TRUE("merge(AB, A) прошёл", ab.mergedWith(ja, &again, nullptr, &err));
    ZT_TRUE("вход поглощён без следа", again.contentDigest() == ab.contentDigest());
}

void checkMergeSelfIsIdentity() {
    Builder a;
    a.add(ZJournal::NewRecord::save(note("раз"), ZJournal::Stamp::at(kNow)));
    a.add(ZJournal::NewRecord::tombstone(ZJournal::Stamp::at(kNow + 500)));
    const ZJournal ja = a.parsed();

    ZJournal m;
    QString err;
    ZT_TRUE("merge(A,A) прошёл", ja.mergedWith(ja, &m, nullptr, &err));
    ZT_TRUE("merge(A,A) == A по набору", m.contentDigest() == ja.contentDigest());

    // И с пустым журналом union равен непустому входу.
    ZJournal empty, m2;
    ZT_TRUE("merge(A, пусто) прошёл", ja.mergedWith(empty, &m2, nullptr, &err));
    ZT_TRUE("пустой не прибавил и не убавил", m2.contentDigest() == ja.contentDigest());
}

// Долг отчёта 3: журнал, вычищенный гашением, слитый с досодержавшей его
// копией, не воскрешает погашенное.
void checkCleanedDoesNotResurrect() {
    Builder full;
    full.add(ZJournal::NewRecord::save(note("раз"), ZJournal::Stamp::at(kNow)));
    full.add(ZJournal::NewRecord::save(note("два"), ZJournal::Stamp::at(kNow + 1000)));
    full.add(ZJournal::NewRecord::save(note("три"), ZJournal::Stamp::at(kNow + 2000)));
    full.add(ZJournal::NewRecord::save(note("четыре"), ZJournal::Stamp::at(kNow + 3000)));
    const Builder oldCopy = full;  // копия уехала на другое устройство ДО чистки

    // Чистка: записи «два» и «три» гасятся адресом...
    ZJournal before = full.parsed();
    QVector<ZJournal::RecordRef> refs{
        ZJournal::RecordRef(before.at(1).time(), before.at(1).digest()),
        ZJournal::RecordRef(before.at(2).time(), before.at(2).digest()),
    };
    full.add(ZJournal::NewRecord::amendment(ZJournal::Stamp::at(kNow + 4000)).voiding(refs));

    // ...и выбрасываются физически — как делает пересборка файла хранилищем.
    ZJournal withAmendment = full.parsed();
    QVector<int> keep;
    for (int i = 0; i < withAmendment.size(); ++i)
        if (!withAmendment.isVoided(i)) keep.append(i);
    QByteArray cleanedBytes;
    QString err;
    ZT_TRUE("чистка пересобрала файл",
            withAmendment.toBytes(keep, withAmendment.cleanVersion(), &cleanedBytes, &err));
    ZJournal cleaned;
    ZT_TRUE("вычищенный разобрался", cleaned.parse(cleanedBytes, ZJournal::Want::All, 0, &err));
    ZT_EQ("в вычищенном три записи", num(3), num(cleaned.size()));

    ZJournal merged;
    ZJournal::MergeStats st;
    ZT_TRUE("слияние со старой копией прошло",
            cleaned.mergedWith(oldCopy.parsed(), &merged, &st, &err));
    ZT_EQ("вернувшиеся погашены и выброшены", num(2), num(st.voidedDropped));
    for (int i = 0; i < merged.size(); ++i) {
        ZT_TRUE("«два» не воскресло", merged.at(i).digest() != digestOf("два"));
        ZT_TRUE("«три» не воскресло", merged.at(i).digest() != digestOf("три"));
    }
    ZT_TRUE("итог равен вычищенному — заливать нечего",
            merged.contentDigest() == cleaned.contentDigest());
}

void checkFrameConflictDeterministic() {
    // Одно содержимое в одну миллисекунду, но с разной ревизией: адрес один,
    // рамки разные. Победитель обязан быть общим для обоих направлений.
    Builder a;
    a.add(ZJournal::NewRecord::save(note("икс"), ZJournal::Stamp::at(kNow)));  // seq 1

    Builder b;
    b.add(ZJournal::NewRecord::save(note("игрек"), ZJournal::Stamp::at(kNow - 1000)));
    b.add(ZJournal::NewRecord::save(note("икс"), ZJournal::Stamp::at(kNow)));  // seq 2

    ZJournal ab, ba;
    ZJournal::MergeStats st;
    QString err;
    ZT_TRUE("A+B слились", a.parsed().mergedWith(b.parsed(), &ab, &st, &err));
    ZT_TRUE("B+A слились", b.parsed().mergedWith(a.parsed(), &ba, nullptr, &err));
    ZT_EQ("конфликт рамок один", num(1), num(st.frameConflicts));
    ZT_EQ("записей в union две", num(2), num(ab.size()));
    ZT_TRUE("побайтовая коммутативность при конфликте", serialized(ab) == serialized(ba));

    int found = -1;
    for (int i = 0; i < ab.size(); ++i)
        if (ab.at(i).digest() == digestOf("икс")) found = i;
    ZT_TRUE("запись «икс» на месте", found >= 0);
    if (found >= 0)
        ZT_EQ("победила большая ревизия", num(2), num(ab.at(found).seq()));
}

void checkDamagedInputRefused() {
    Builder good;
    good.add(ZJournal::NewRecord::save(note("раз"), ZJournal::Stamp::at(kNow)));
    good.add(ZJournal::NewRecord::save(note("два"), ZJournal::Stamp::at(kNow + 1000)));

    // Ищем переворот бита, который «переживает» разбор: запись на месте, но
    // сумма рамки не сошлась. Именно такой вход слияние обязано отвергнуть.
    ZJournal damaged;
    bool made = false;
    const QByteArray original = good.bytes;
    for (int off = 0; off < original.size() && !made; ++off) {
        QByteArray bent = original;
        bent[off] = char(bent[off] ^ 0x01);
        ZJournal j;
        QString err;
        if (!j.parse(bent, ZJournal::Want::All, 0, &err)) continue;
        if (j.damagedCount() == 1 && j.size() == 2 && !j.tailTrimmed()) {
            damaged = j;
            made = true;
        }
    }
    ZT_TRUE("испорченная запись изготовилась", made);
    if (!made) return;

    ZJournal out;
    QString err;
    ZT_TRUE("слияние с порчей слева отвергнуто",
            !damaged.mergedWith(good.parsed(), &out, nullptr, &err));
    ZT_TRUE("причина названа", !err.isEmpty());
    ZT_TRUE("слияние с порчей справа отвергнуто",
            !good.parsed().mergedWith(damaged, &out, nullptr, &err));
}

void checkCleanVersionSurvivesOnlyWhenEqual() {
    Builder clean;  // "0.1"
    clean.add(ZJournal::NewRecord::save(note("раз"), ZJournal::Stamp::at(kNow)));
    Builder legacy((QString()));  // v0
    legacy.add(ZJournal::NewRecord::save(note("два"), ZJournal::Stamp::at(kNow + 1000)));

    ZJournal m;
    QString err;
    ZT_TRUE("разные версии чистки слились", clean.parsed().mergedWith(legacy.parsed(), &m, nullptr, &err));
    ZT_TRUE("итог честно нечищен", m.cleanVersion().isEmpty());

    ZJournal m2;
    ZT_TRUE("равные версии слились", clean.parsed().mergedWith(clean.parsed(), &m2, nullptr, &err));
    ZT_EQ("равная версия выжила", std::string(ZJournal::kCleanVersion),
          m2.cleanVersion().toStdString());
}

// --- 3. property: случайные пары журналов ----------------------------------

void checkMergePropertiesOnRandom(unsigned seed) {
    std::mt19937 rng(seed);
    fprintf(stderr, "journal_merge: зерно %u\n", seed);

    const char* marks[] = {"альфа", "бета", "гамма", "дельта", "эпсилон",
                           "дзета", "эта", "тета"};
    int failuresBefore = zt::g_failures;

    for (int round = 0; round < 40 && zt::g_failures == failuresBefore; ++round) {
        // Времена берутся из тесной сетки: совпадения между ветками — не
        // редкий случай, а норма этого набора.
        auto stampAt = [&](qint64 base) {
            return ZJournal::Stamp::at(base + qint64(rng() % 12) * 1000);
        };
        auto randomOp = [&](Builder* into, qint64 base) {
            const unsigned dice = rng() % 100;
            const char* mark = marks[rng() % 8];
            if (dice < 55) {
                into->add(ZJournal::NewRecord::save(note(mark), stampAt(base)));
            } else if (dice < 70) {
                into->add(ZJournal::NewRecord::external(note(mark), stampAt(base)));
            } else if (dice < 80) {
                into->add(ZJournal::NewRecord::tombstone(stampAt(base)));
            } else if (dice < 90) {
                const ZJournal j = into->parsed();
                if (j.size() > 0) {
                    const Record& src = j.at(int(rng() % unsigned(j.size())));
                    into->add(ZJournal::NewRecord::restore(note(mark), src.time(), stampAt(base)));
                } else {
                    into->add(ZJournal::NewRecord::save(note(mark), stampAt(base)));
                }
            } else {
                // Гашение случайной живой записи — как «набрал и отменил».
                const ZJournal j = into->parsed();
                QVector<ZJournal::RecordRef> refs;
                if (j.size() > 0) {
                    const Record& victim = j.at(int(rng() % unsigned(j.size())));
                    if (victim.hasSnapshot())
                        refs.append(ZJournal::RecordRef(victim.time(), victim.digest()));
                }
                if (!refs.isEmpty())
                    into->add(ZJournal::NewRecord::amendment(stampAt(base)).voiding(refs));
                else
                    into->add(ZJournal::NewRecord::save(note(mark), stampAt(base)));
            }
        };

        Builder common;
        const int prefix = int(rng() % 5);
        for (int i = 0; i < prefix; ++i) randomOp(&common, kNow + i * 60'000);
        Builder a = common;
        Builder b = common;
        const int opsA = int(rng() % 6);
        const int opsB = int(rng() % 6);
        for (int i = 0; i < opsA; ++i) randomOp(&a, kNow + (prefix + i) * 60'000);
        for (int i = 0; i < opsB; ++i) randomOp(&b, kNow + (prefix + i) * 60'000);

        const ZJournal ja = a.parsed();
        const ZJournal jb = b.parsed();
        ZJournal ab, ba;
        ZJournal::MergeStats st;
        QString err;
        ZT_TRUE(("раунд " + std::to_string(round) + ": A+B слились: " + err.toStdString()).c_str(),
                ja.mergedWith(jb, &ab, &st, &err));
        ZT_TRUE(("раунд " + std::to_string(round) + ": B+A слились").c_str(),
                jb.mergedWith(ja, &ba, nullptr, &err));

        checkSuperset("union покрывает A", ab, ja);
        checkSuperset("union покрывает B", ab, jb);
        ZT_TRUE("счётчики сходятся с размером",
                st.fromThis + st.fromOther + st.common == ab.size() + st.voidedDropped);
        ZT_TRUE("merge(A,B) == merge(B,A) побайтово", serialized(ab) == serialized(ba));

        // Слитое переживает укладку и разбор без потерь набора.
        ZJournal re;
        ZT_TRUE("слитое разобралось обратно",
                re.parse(serialized(ab), ZJournal::Want::All, 0, &err));
        ZT_TRUE("набор пережил укладку", re.contentDigest() == ab.contentDigest());

        // Поглощение входов: повторное слияние ничего не меняет.
        ZJournal again;
        ZT_TRUE("merge(AB, B) прошёл", ab.mergedWith(jb, &again, nullptr, &err));
        ZT_TRUE("повторное слияние — тождество", again.contentDigest() == ab.contentDigest());

        if (zt::g_failures != failuresBefore)
            fprintf(stderr, "journal_merge: расхождение в раунде %d, зерно %u\n", round, seed);
    }
}

}  // namespace

static int ztRunSuite(int argc, char** argv) {
    (void)argc;
    (void)argv;
    checkContentDigestIgnoresFileOrder();
    checkContentDigestSeesEveryFrameField();
    checkContentDigestGrowsWithRecords();
    checkMergeUnionAndCommutes();
    checkMergeSelfIsIdentity();
    checkCleanedDoesNotResurrect();
    checkFrameConflictDeterministic();
    checkDamagedInputRefused();
    checkCleanVersionSurvivesOnlyWhenEqual();
    unsigned seed = 20260824u;
    if (argc > 1) seed = unsigned(std::stoul(argv[1]));
    checkMergePropertiesOnRandom(seed);
    return zt::report("journal_merge");
}

TEST(JournalMerge, All) {
    std::vector<QByteArray> ztArgs{QByteArrayLiteral("journal_merge_test")};
    std::vector<char*> ztArgv;
    for (QByteArray& a : ztArgs) ztArgv.push_back(a.data());
    EXPECT_EQ(0, ztRunSuite(int(ztArgv.size()), ztArgv.data()));
}
