// Поиск по истории одной заметки.
//
// Что здесь проверяется и почему именно это:
//   - находится текст, которого в живой заметке давно НЕТ — ради этого поиск и
//     заведён;
//   - находка адресуется парой (время, отпечаток), и по ней открывается тот
//     самый слепок: номер записи адресом быть не может, чистка журнала его
//     сдвигает;
//   - ПОИСК СПЕРВА ЧИСТИТ ЖУРНАЛ: обращение к истории пер-заметочное, а искать
//     по дубликатам значит трижды показать одно и то же;
//   - smart case и порог в два знака — те же, что во всём поиске: правила
//     поиска не должны зависеть от того, где ищут.

#include "editor_widget.h"
#include "history_rig.h"
#include "history_search.h"
#include "journal.h"
#include "search.h"

#include "test_util.h"

#include <vector>
#include "testdata.h"

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QTemporaryDir>

#include <string>

using namespace zametti;

namespace {

template <typename T>
std::string num(T value) { return std::to_string(value); }

QString g_root;

QByteArray note(const char* body, const char* stamp) {
    return QByteArray("<!-- zametti\ncreated: 2026-01-01T00:00:00Z\nmodified: ") + stamp +
           "\n-->\n\n" + body;
}

// Заметка с историей из трёх записей; в средней есть слово, которого в живой
// версии нет.
QString makeNote(const QString& id) {
    QDir().mkpath(g_root + QStringLiteral("/.zametti"));
    QDir().mkpath(g_root + QStringLiteral("/history"));
    const QString path = g_root + QLatin1Char('/') + id + QStringLiteral(".md");
    const QByteArray last = note("# Планы\n\nПоехали в Суздаль.\n", "c");
    QFile file(path);
    if (file.open(QIODevice::WriteOnly)) file.write(last);
    file.close();

    journal::History history(g_root);
    QString error;
    const qint64 now = 1'700'000'000'000LL;
    history.append(id,
                   journal::NewRecord::save(note("# Планы\n\nПока ничего.\n", "a"),
                                            journal::Stamp::at(now)),
                   &error);
    history.append(id,
                   journal::NewRecord::save(note("# Планы\n\nПоехали в Кострому.\n", "b"),
                                            journal::Stamp::at(now + 60'000)),
                   &error);
    history.append(id, zametti::journal::NewRecord::save(last, journal::Stamp::at(now + 120'000)), &error);
    return path;
}

void checkFindsWhatIsGone() {
    const QString id = QStringLiteral("01ssssssssss01");
    makeNote(id);
    const journal::History history(g_root);
    const HistorySearchReport report =
        searchNoteHistory(history, id, makeQuery(QStringLiteral("Кострому")));

    ZT_EQ("просмотрены все слепки", num(3), num(report.snapshots));
    ZT_EQ("нашлось в одном", num(1), num(report.withHits));
    ZT_EQ("одна находка", num(1), num(report.hits.size()));
    if (report.hits.isEmpty()) return;
    ZT_TRUE("строка находки — та самая: «" + report.hits[0].line.toStdString() + "»",
            report.hits[0].line.contains(QStringLiteral("Кострому")));
    ZT_TRUE("у находки есть время слепка", report.hits[0].snapshotTime > 0);
    ZT_TRUE("и отпечаток", !report.hits[0].snapshotDigest.empty());

    // Адресация: по паре (время, отпечаток) находится ровно та запись.
    journal::ZJournal journal;
    QString error;
    ZT_TRUE("журнал читается", history.read(id, &journal, &error));
    const int at = journal.indexOf(report.hits[0].snapshotTime,
                                         report.hits[0].snapshotDigest);
    ZT_EQ("вешка ведёт к средней записи", num(1), num(at));
    QByteArray body;
    ZT_TRUE("слепок собирается", history.snapshotAt(id, at, &body, &error));
    ZT_TRUE("и в нём то самое слово", body.contains("Кострому"));
}

// Свежие слепки первыми: человек ищет «где это было», и ближнее прошлое нужнее.
void checkFreshFirst() {
    const QString id = QStringLiteral("01ssssssssss02");
    makeNote(id);
    const journal::History history(g_root);
    const HistorySearchReport report =
        searchNoteHistory(history, id, makeQuery(QStringLiteral("Планы")));
    ZT_TRUE("нашлось во всех трёх: " + num(report.withHits), report.withHits == 3);
    bool descending = true;
    for (int i = 1; i < report.hits.size(); ++i)
        descending = descending && report.hits[i].snapshotTime <= report.hits[i - 1].snapshotTime;
    ZT_TRUE("свежие слепки идут первыми", descending);
}

// Правила поиска общие: два знака и smart case.
void checkQueryRules() {
    const QString id = QStringLiteral("01ssssssssss03");
    makeNote(id);
    const journal::History history(g_root);
    ZT_TRUE("один знак не ищется",
            searchNoteHistory(history, id, makeQuery(QStringLiteral("П"))).hits.isEmpty());
    ZT_TRUE("строчными находит и с заглавной",
            !searchNoteHistory(history, id, makeQuery(QStringLiteral("кострому")))
                 .hits.isEmpty());
    ZT_TRUE("а с заглавной — только точное",
            searchNoteHistory(history, id, makeQuery(QStringLiteral("КОСТРОМУ")))
                .hits.isEmpty());
}

// ПОИСК ЧИСТИТ ЖУРНАЛ ПЕРЕД ПРОХОДОМ. Заводим журнал с дубликатами (как писала
// программа до этапа 10) и ищем через редактор: находок обязано быть столько,
// сколько РАЗНЫХ состояний, а не сколько записей.
void checkSearchMigratesFirst() {
    const QString id = QStringLiteral("01ssssssssss04");
    const QString path = g_root + QLatin1Char('/') + id + QStringLiteral(".md");
    const QByteArray a = note("# Планы\n\nПоехали в Кострому.\n", "a");
    const QByteArray b = note("# Планы\n\nПоехали в Кострому и обратно, длинная дописка.\n", "b");
    const QByteArray a2 = note("# Планы\n\nПоехали в Кострому.\n", "c");
    {
        QFile file(path);
        if (file.open(QIODevice::WriteOnly)) file.write(a2);
    }
    journal::History history(g_root);
    QString error;
    const qint64 now = 1'700'000'000'000LL;
    history.append(id, zametti::journal::NewRecord::save(a, journal::Stamp::at(now)), &error);
    history.append(id, zametti::journal::NewRecord::save(b, journal::Stamp::at(now + 60'000)), &error);
    history.append(id, zametti::journal::NewRecord::save(a2, journal::Stamp::at(now + 120'000)), &error);
    // Шапку — на старый лад: иначе журнал уже считается чищеным.
    {
        QFile file(history.pathFor(id));
        ZT_TRUE("журнал открыт", file.open(QIODevice::ReadOnly));
        QByteArray blob = file.readAll();
        file.close();
        const QByteArray clean =
            journal::ZJournal::headerBytes(QString::fromLatin1(journal::kCleanVersion));
        blob = journal::ZJournal::headerBytes(QString()) + blob.mid(clean.size());
        QFile out(history.pathFor(id));
        ZT_TRUE("журнал переписан", out.open(QIODevice::WriteOnly | QIODevice::Truncate));
        out.write(blob);
    }

    journal::ZJournal before;
    ZT_TRUE("грязный журнал читается", history.read(id, &before, &error));
    ZT_EQ("в нём три записи", num(3), num(before.size()));

    NoteEditor editor;
    editor.setStoreRoot(g_root);
    editor.openFile(path);
    zt::HistoryRig rig(editor);
    const HistorySearchReport report = rig.controller.searchHistory(QStringLiteral("Кострому"));

    journal::ZJournal after;
    ZT_TRUE("журнал читается и после", history.read(id, &after, &error));
    ZT_EQ("поиск вычистил дубликат", num(1), num(after.size()));
    ZT_EQ("и находка одна, а не три", num(1), num(report.hits.size()));
}

// --- прибор ------------------------------------------------------------------
//
// Не проверка, а замер: сколько стоит проход по истории ОДНОЙ заметки. На нём
// стоит решение не заводить ради этого поиска отдельный поток (в отличие от
// поиска по хранилищу). Запускается руками:
//
//   taskset -c 0 ./history_search_test --bench <хранилище> [слово]
//
// Рядом печатается ЭТАЛОННЫЙ счётный цикл: пока он стоит намертво, разброс в
// замерах — свойство измеряемого кода, а поехал он — поехала машина.
qint64 yardstick() {
    QElapsedTimer clock;
    clock.start();
    volatile double sum = 0;
    for (int i = 0; i < 20'000'000; ++i) sum += double(i % 7);
    return clock.nsecsElapsed() / 1000;
}

void bench(const QString& root, const QString& needle) {
    const journal::History history(root);
    const QDir dir(QDir(root).filePath(QStringLiteral("history")));
    const QStringList files = dir.entryList({QStringLiteral("*.log")}, QDir::Files, QDir::Name);
    const Query query = makeQuery(needle);

    std::printf("эталон до: %lld мкс\n", (long long)yardstick());
    qint64 worst = 0;
    QString worstId;
    qint64 total = 0;
    int snapshots = 0;
    for (const QString& name : files) {
        const QString id = name.left(name.size() - 4);
        // Минимум по трём проходам: греет кэш файловой системы первый, а нам
        // нужна цена работы, а не цена холодного чтения.
        qint64 best = -1;
        int found = 0;
        for (int round = 0; round < 3; ++round) {
            QElapsedTimer clock;
            clock.start();
            const HistorySearchReport report = searchNoteHistory(history, id, query);
            const qint64 spent = clock.nsecsElapsed() / 1000;
            if (best < 0 || spent < best) best = spent;
            found = report.snapshots;
        }
        snapshots += found;
        total += best;
        if (best > worst) {
            worst = best;
            worstId = id;
        }
    }
    std::printf("журналов %lld, слепков %d, всего %.1f мс, худший %s — %lld мкс\n",
                (long long)files.size(), snapshots, double(total) / 1000.0,
                worstId.toUtf8().constData(), (long long)worst);
    std::printf("эталон после: %lld мкс\n", (long long)yardstick());
}

}  // namespace

static int ztRunSuite(int argc, char** argv) {
    if (argc > 2 && std::string(argv[1]) == "--bench") {
        bench(QString::fromLocal8Bit(argv[2]),
              argc > 3 ? QString::fromLocal8Bit(argv[3]) : QStringLiteral("что"));
        return 0;
    }
    QTemporaryDir tmp;
    if (!tmp.isValid()) {
        std::printf("не завёлся временный каталог\n");
        return 2;
    }
    g_root = tmp.path();

    checkFindsWhatIsGone();
    checkFreshFirst();
    checkQueryRules();
    checkSearchMigratesFirst();

    return zt::report("поиск по истории");
}

// Набор целиком одним TEST: тело не тронуто, argv ему собран здесь.
// Дробить на отдельные проверки — отдельная работа, по одному набору.
TEST(HistorySearch, All) {
    std::vector<QByteArray> ztArgs{QByteArrayLiteral("history_search_test")};
    std::vector<char*> ztArgv;
    for (QByteArray& a : ztArgs) ztArgv.push_back(a.data());
    EXPECT_EQ(0, ztRunSuite(int(ztArgv.size()), ztArgv.data()));
}
