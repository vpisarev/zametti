// Ленивая чистка истории: версия журнала, правила пересборки, идемпотентность.
//
// Что здесь проверяется и почему именно это:
//   - фикстура ИЗ ЖИЗНИ: настоящий журнал владельца с настоящими дубликатами;
//     ожидаемый результат схлопывания записан явно, числом и временами;
//   - границы неприкосновенны: опорная запись, надгробие, последняя запись, и
//     вешка External, через которую схлопывание не перепрыгивает;
//   - ЛЕНИВОСТЬ: журнал версии 0.1 без форса не трогается ни байтом;
//   - ИДЕМПОТЕНТНОСТЬ: повторный форс не меняет ни байта;
//   - сторож свежести — единственное, чем миграция отличается от живой
//     записи: одни и те же записи, поданные живым путём и вычищенные
//     миграцией, дают одинаковый журнал.
//
// Фикстура лежит в .testdata/ (в репозиторий корпуса не кладутся). Её нет —
// проверка по ней громко пропускается, остальные идут.

#include "history_rules.h"
#include "journal.h"

#include "test_util.h"

#include <vector>
#include "testdata.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include <string>

using namespace zametti;
using zametti::journal::Kind;

namespace {

std::string str(const QString& s) { return s.toStdString(); }
template <typename T>
std::string num(T value) { return std::to_string(value); }

constexpr qint64 kNow = 1'700'000'000'000LL;
constexpr qint64 kMinute = 60 * 1000;
constexpr qint64 kHour = 60 * kMinute;

QString g_fixture;   // .testdata/journal-fixture

// Тело заметки со штампом modified: миграция обязана считать две такие
// одинаковыми, если различаются только штампы.
QByteArray body(const char* text, int stamp) {
    QByteArray out = "<!-- zametti\ncreated: 2026-07-31T00:00:00Z\nmodified: 2026-08-0";
    out += QByteArray::number(stamp);
    out += "T00:00:00Z\n-->\n\n# Заметка\n\n";
    out += text;
    out += "\n";
    return out;
}

QByteArray fileBytes(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return file.readAll();
}

// Журнал, какие писала программа ДО этапа 10: та же последовательность записей,
// но в шапке нет ключа версии содержимого. Иначе миграцию не на чем проверять —
// нынешний append заводит журнал сразу чищеным.
void makeV0(const QString& path) {
    QByteArray bytes = fileBytes(path);
    const QByteArray clean = journal::ZJournal::headerBytes(QString::fromLatin1(journal::kCleanVersion));
    const QByteArray old = journal::ZJournal::headerBytes(QString());
    ZT_TRUE("журнал начинается нынешней шапкой", bytes.startsWith(clean));
    bytes = old + bytes.mid(clean.size());
    QFile file(path);
    ZT_TRUE("журнал открыт на запись", file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    file.write(bytes);
    file.close();
}

// Что дописать — по роду записи. Наборам удобно перечислять роды, а
// именованные создатели не дают собрать неверное сочетание.
journal::NewRecord recordFor(Kind kind, qint64 time, const QByteArray& body, qint64 source) {
    const journal::Stamp when = journal::Stamp::at(time);
    switch (kind) {
        case Kind::External: return journal::NewRecord::external(body, when);
        case Kind::Restore: return journal::NewRecord::restore(body, source, when);
        case Kind::Tombstone: return journal::NewRecord::tombstone(when);
        case Kind::Amendment: return journal::NewRecord::amendment(when);
        case Kind::Save: break;
    }
    return journal::NewRecord::save(body, when);
}

QString append(journal::History& h, const QString& id, Kind kind, qint64 time,
               const QByteArray& snapshot, qint64 source = 0) {
    QString error;
    if (!h.append(id, recordFor(kind, time, snapshot, source), &error)) return error;
    return {};
}

// Времена уцелевших записей — то, чем удобнее всего описать «что осталось».
std::string timesOf(journal::History& h, const QString& id, qint64 base) {
    journal::ZJournal j;
    QString error;
    if (!h.read(id, &j, &error)) return str(error);
    std::string out;
    for (int i = 0; i < j.size(); ++i) {
        const journal::ZJournal::Entry& e = j.at(i);
        // Вешки, а не записи: гашение и погашенное человеку не показываются, и
        // сходиться живому пути с миграцией положено именно по вешкам. Живой
        // путь гасит адресом (запись гашения остаётся и едет в облако),
        // миграция чистит насовсем — это разовая уборка журналов, которых в
        // облаке ещё не было.
        if (!e.statesContent() || j.isVoided(i)) continue;
        if (!out.empty()) out += " ";
        out += std::to_string((e.time() - base) / kMinute);
        if (e.kind() != Kind::Save) out += e.kind() == Kind::Tombstone ? "T" : "X";
    }
    return out;
}

// Прогнать чистку и сказать, что вышло.
history::Report compress(journal::History& h, const QString& id, bool force) {
    history::Report report;
    QString error;
    ZT_TRUE("чистка прошла", history::compressJournal(h, id, {}, force, &report, &error));
    if (!error.isEmpty()) std::fprintf(stderr, "  (%s)\n", error.toUtf8().constData());
    return report;
}

// --- фикстура из жизни ------------------------------------------------------

void checkRealJournal() {
    if (g_fixture.isEmpty() || !QFileInfo::exists(g_fixture)) {
        std::fprintf(stderr,
                     "ПРОПУЩЕНО: фикстуры «Пробуем Obsidian» нет (%s).\n"
                     "  Она живёт в .testdata/, а корпуса в репозиторий не кладутся.\n",
                     g_fixture.toUtf8().constData());
        return;
    }
    QTemporaryDir dir;
    const QString id = QStringLiteral("01n6cqevtnwrf8");
    QDir().mkpath(QDir(dir.path()).filePath(QStringLiteral("history")));
    const QString path = QDir(dir.path()).filePath(QStringLiteral("history/%1.log").arg(id));
    QFile::copy(QDir(g_fixture).filePath(id + QStringLiteral(".log")), path);

    journal::History h(dir.path());
    journal::ZJournal before;
    QString error;
    ZT_TRUE("журнал владельца читается", h.read(id, &before, &error));
    ZT_EQ("он не чищен (v0)", std::string(), str(before.cleanVersion()));
    ZT_EQ("записей в нём", num(11), num(before.size()));

    QByteArray lastBefore;
    ZT_TRUE("последний слепок собирается",
            h.snapshotAt(id, int(before.size()) - 1, &lastBefore, &error));

    const history::Report report = compress(h, id, false);
    ZT_EQ("версия была", std::string(), str(report.versionBefore));
    ZT_EQ("версия стала", std::string("0.1"), str(report.versionAfter));
    ZT_EQ("записей было", num(11), num(report.recordsBefore));
    // ВОТ ОЖИДАЕМОЕ СХЛОПЫВАНИЕ, записанное явно: одиннадцать записей, шесть из
    // которых — возвраты к уже записанному состоянию (человек набирал и
    // отменял), сходятся к пяти.
    ZT_EQ("записей стало", num(5), num(report.recordsAfter));
    ZT_EQ("и все шесть ушли дубликатами", num(6), num(report.duplicates));
    ZT_EQ("схлопнутых мелких правок нет", num(0), num(report.merged));
    ZT_TRUE("файл переписан", report.rewritten);

    journal::ZJournal after;
    ZT_TRUE("чищеный журнал читается", h.read(id, &after, &error));
    ZT_EQ("версия в шапке", std::string("0.1"), str(after.cleanVersion()));
    ZT_EQ("уцелевшие записи (минуты от первой)", std::string("0 15680 15850 15872 15880"),
          timesOf(h, id, before.at(0).time()));

    // СОСТОЯНИЕ ЗАМЕТКИ НЕ ПОТЕРЯНО. Именно состояние, а не байты: из пары
    // одинаковых записей остаётся САМАЯ СТАРАЯ, и штамп modified в ней —
    // её собственный, на 39 секунд раньше. Текст при этом тот же до знака.
    QByteArray lastAfter;
    ZT_TRUE("последний слепок собирается и после",
            h.snapshotAt(id, int(after.size()) - 1, &lastAfter, &error));
    ZT_TRUE("последнее состояние то же самое", sameApartFromModified(lastBefore, lastAfter));
    ZT_TRUE("а байты — от старшей из равных записей", lastBefore != lastAfter);

    // Равных записей в чищеном журнале не осталось ни одной пары.
    bool anyEqual = false;
    for (int i = 0; i < after.size(); ++i)
        for (int j = i + 1; j < after.size(); ++j) {
            QByteArray a, b;
            if (h.snapshotAt(id, i, &a, &error) && h.snapshotAt(id, j, &b, &error))
                anyEqual = anyEqual || sameApartFromModified(a, b);
        }
    ZT_TRUE("равных записей не осталось", !anyEqual);

    // ИДЕМПОТЕНТНОСТЬ на живом журнале: повторный форс не меняет ни байта.
    const QByteArray bytes = fileBytes(path);
    const history::Report again = compress(h, id, true);
    ZT_TRUE("повторный форс ничего не переписал", !again.rewritten);
    ZT_TRUE("и файл побайтово тот же", fileBytes(path) == bytes);
}

// --- границы ----------------------------------------------------------------

// Возврат к уже записанному состоянию: A, B, A' → остаётся A.
void checkReturnCollapses(const QString& root) {
    journal::History h(root);
    const QString id = QStringLiteral("01n6r08s8wy52a");
    append(h, id, Kind::Save, kNow, body("раз", 1));
    append(h, id, Kind::Save, kNow + kMinute, body("два больше на много знаков и ещё", 2));
    append(h, id, Kind::Save, kNow + 2 * kMinute, body("раз", 3));
    makeV0(h.pathFor(id));

    const history::Report report = compress(h, id, false);
    ZT_EQ("три записи сошлись к одной", num(1), num(report.recordsAfter));
    ZT_EQ("осталась самая старая из равных", std::string("0"), timesOf(h, id, kNow));
}

// Та же тройка, но между дубликатами стоит чужая вешка: схлопывания нет.
// Проверка парная к предыдущей — без неё не видно, что дело именно в вешке.
void checkExternalStops(const QString& root) {
    journal::History h(root);
    const QString id = QStringLiteral("01n6r08s8wy52b");
    append(h, id, Kind::Save, kNow, body("раз", 1));
    append(h, id, Kind::External, kNow + kMinute,
           body("два больше на много знаков и ещё", 2));
    append(h, id, Kind::Save, kNow + 2 * kMinute, body("раз", 3));
    makeV0(h.pathFor(id));

    const history::Report report = compress(h, id, false);
    ZT_EQ("через External не схлопывается", num(3), num(report.recordsAfter));
    ZT_EQ("и вешка на месте", std::string("0 1X 2"), timesOf(h, id, kNow));
}

// Надгробие и последняя запись неприкосновенны.
void checkTombstoneSurvives(const QString& root) {
    journal::History h(root);
    const QString id = QStringLiteral("01n6r08s8wy52c");
    append(h, id, Kind::Save, kNow, body("раз", 1));
    append(h, id, Kind::Save, kNow + kMinute, body("два больше на много знаков и ещё", 2));
    append(h, id, Kind::Save, kNow + 2 * kMinute, body("раз", 3));
    append(h, id, Kind::Tombstone, kNow + 3 * kMinute, QByteArray());
    makeV0(h.pathFor(id));

    compress(h, id, false);
    ZT_EQ("надгробие и опорная запись целы", std::string("0 3T"), timesOf(h, id, kNow));
}

// Мелкая правка схлопывается, но опорную запись не съедает.
void checkMergeKeepsBaseline(const QString& root) {
    journal::History h(root);
    const QString id = QStringLiteral("01n6r08s8wy52d");
    append(h, id, Kind::Save, kNow, body("текст", 1));
    append(h, id, Kind::Save, kNow + kHour, body("текст с добавкой", 2));
    append(h, id, Kind::Save, kNow + 2 * kHour, body("текст с добавкой и ещё", 3));
    makeV0(h.pathFor(id));

    const history::Report report = compress(h, id, false);
    ZT_EQ("мелкие правки схлопнулись в одну", num(2), num(report.recordsAfter));
    ZT_EQ("и схлопнута ровно одна", num(1), num(report.merged));
    ZT_EQ("опорная запись цела", std::string("0 120"), timesOf(h, id, kNow));
}

// Миграция чистит РЕТРОАКТИВНО: те же три записи, разнесённые на годы.
// Живое схлопывание их бы не тронуло — вот он, единственный разошедшийся сторож.
void checkAgeIgnored(const QString& root) {
    journal::History h(root);
    const QString id = QStringLiteral("01n6r08s8wy52e");
    const qint64 year = 365LL * 24 * kHour;
    append(h, id, Kind::Save, kNow - 3 * year, body("текст", 1));
    append(h, id, Kind::Save, kNow - 2 * year, body("текст с добавкой", 2));
    append(h, id, Kind::Save, kNow - year, body("текст", 3));
    makeV0(h.pathFor(id));

    const history::Report report = compress(h, id, false);
    ZT_EQ("старые дубликаты тоже вычищены", num(1), num(report.recordsAfter));
}

// Адресация записей — по (время, отпечаток), а не по номеру: миграция
// выкидывает дубликаты, и номера съезжают. Вешка на вычищенную запись обязана
// приводить к выжившей равной ей.
void checkAddressByTimeAndHash(const QString& root) {
    journal::History h(root);
    const QString id = QStringLiteral("01n6r08s8wy52g");
    append(h, id, Kind::Save, kNow, body("раз", 1));
    append(h, id, Kind::Save, kNow + kMinute, body("два больше на много знаков и ещё", 2));
    append(h, id, Kind::Save, kNow + 2 * kMinute, body("раз", 3));

    journal::ZJournal before;
    QString error;
    ZT_TRUE("журнал читается", h.read(id, &before, &error));
    // Вешка на последнюю запись — ту самую, которую чистка и выкинет.
    const qint64 markTime = before.at(before.size() - 1).time();
    const Digest markDigest = before.at(before.size() - 1).digest();
    ZT_EQ("до чистки вешка ведёт к ней самой", num(2),
          num(before.indexOf(markTime, markDigest)));

    makeV0(h.pathFor(id));
    compress(h, id, false);

    journal::ZJournal after;
    ZT_TRUE("и после чистки читается", h.read(id, &after, &error));
    ZT_EQ("после чистки — к выжившей равной", num(0),
          num(after.indexOf(markTime, markDigest)));
    QByteArray target;
    ZT_TRUE("слепок по этой вешке собирается",
            h.snapshotAt(id, after.indexOf(markTime, markDigest), &target, &error));
    ZT_TRUE("и содержимое у него то самое", sameApartFromModified(target, body("раз", 3)));
}

// Ленивость: чищеный журнал без форса не трогается ни байтом.
void checkCleanLeftAlone(const QString& root) {
    journal::History h(root);
    const QString id = QStringLiteral("01n6r08s8wy52f");
    append(h, id, Kind::Save, kNow, body("раз", 1));
    append(h, id, Kind::Save, kNow + kMinute, body("два больше на много знаков и ещё", 2));
    const QByteArray bytes = fileBytes(h.pathFor(id));

    const history::Report report = compress(h, id, false);
    ZT_EQ("новый журнал заведён сразу чищеным", std::string("0.1"), str(report.versionBefore));
    ZT_TRUE("и не переписан", !report.rewritten);
    ZT_TRUE("байт в байт тот же", fileBytes(h.pathFor(id)) == bytes);
}

// Живая запись и миграция расходятся ТОЛЬКО сторожем свежести. Одни и те же
// свежие правки: одни поданы правилом по одной, другие свалены в журнал как
// есть и вычищены миграцией. Журналы обязаны сойтись.
void checkLiveAndMigrationAgree(const QString& root) {
    const QVector<QByteArray> steps = {
        body("начало", 1),
        body("начало и продолжение подлиннее, чтобы не схлопнулось", 2),
        body("начало", 3),
        body("начало и продолжение подлиннее, чтобы не схлопнулось", 4),
        body("совсем другое содержимое, ни на что не похожее вовсе", 5),
    };

    journal::History h(root);
    const QString live = QStringLiteral("01n6r08s8wy521");
    const QString raw = QStringLiteral("01n6r08s8wy522");

    // Живой путь: то же решение, что принимает автосохранение.
    history::Rules rules;
    for (int i = 0; i < steps.size(); ++i) {
        const qint64 when = kNow + i * kMinute;
        journal::ZJournal j;
        QString error;
        h.read(live, &j, &error);
        const auto snapshotOf = [&](int at) {
            QByteArray out;
            QString why;
            if (!h.snapshotAt(live, at, &out, &why)) return QByteArray();
            return out;
        };
        const history::Step step =
            history::decideStep(j, snapshotOf, steps[i], Kind::Save, when, rules);
        // Живой путь гасит адресом — ровно то же, что делает запись заметки.
        QVector<journal::EntryRef> voids;
        for (int at : step.voided) voids.append(journal::EntryRef(j.at(at).time(), j.at(at).digest()));
        if (step.writeNew)
            h.append(live, journal::NewRecord::save(steps[i], journal::Stamp::at(when)).voiding(voids),
                     &error);
        else if (!voids.isEmpty())
            h.append(live, journal::NewRecord::amendment(journal::Stamp::at(when)).voiding(voids),
                     &error);
    }

    // Сырой путь: всё подряд, как писала программа до этапа 9.
    for (int i = 0; i < steps.size(); ++i)
        append(h, raw, Kind::Save, kNow + i * kMinute, steps[i]);
    makeV0(h.pathFor(raw));
    compress(h, raw, false);

    ZT_EQ("живая запись и миграция сошлись", timesOf(h, live, kNow), timesOf(h, raw, kNow));
}

}  // namespace

static int ztRunSuite(int argc, char** argv) {
    if (argc > 1) g_fixture = QString::fromLocal8Bit(argv[1]);

    QTemporaryDir dir;
    QDir().mkpath(QDir(dir.path()).filePath(QStringLiteral("history")));

    checkRealJournal();
    checkReturnCollapses(dir.path());
    checkExternalStops(dir.path());
    checkTombstoneSurvives(dir.path());
    checkMergeKeepsBaseline(dir.path());
    checkAgeIgnored(dir.path());
    checkAddressByTimeAndHash(dir.path());
    checkCleanLeftAlone(dir.path());
    checkLiveAndMigrationAgree(dir.path());

    return zt::report("history-compress");
}

// Набор целиком одним TEST: тело не тронуто, argv ему собран здесь.
// Дробить на отдельные проверки — отдельная работа, по одному набору.
TEST(HistoryCompress, All) {
    std::vector<QByteArray> ztArgs{QByteArrayLiteral("history_compress_test")};
    ztArgs.push_back((zt::TestData::corpus(QStringLiteral("journal-fixture"))).toLocal8Bit());
    std::vector<char*> ztArgv;
    for (QByteArray& a : ztArgs) ztArgv.push_back(a.data());
    EXPECT_EQ(0, ztRunSuite(int(ztArgv.size()), ztArgv.data()));
}
