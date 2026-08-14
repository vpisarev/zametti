// Фаззинг журнала: случайная череда правок, сохранений, восстановлений,
// удалений и прореживаний — и после каждого шага проверяются инварианты.
//
// Что стережётся (имена из брифа этапа 7):
//   B. между прореживаниями файл строго растёт;
//   C. ни одна операция журнал не укорачивает; прореживание — единственное
//      исключение, и оно объявлено;
//   + каждый уцелевший слепок собирается и сходится со своим отпечатком;
//   + последняя запись выживает всегда;
//   + прореживание идемпотентно: второй прогон не меняет ни байта.
//
// Зерно задаётся аргументом и печатается: упавший прогон обязан повторяться.
// Без аргумента берётся постоянное — набор в ctest должен быть одинаковым от
// прогона к прогону, иначе красный тест ничего не доказывает.

#include "journal.h"

#include "test_util.h"

#include <vector>
#include "testdata.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <random>
#include <string>

using namespace zametti;
using zametti::journal::Kind;

namespace {

std::string num(qint64 v) { return std::to_string(v); }

// Правдоподобная заметка: разметка, кириллица, отступы.
QByteArray noteAt(int step, int lines) {
    QByteArray out = "<!-- zametti\ncreated: 2023-06-02T23:00:02Z\n-->\n\n# Заметка\n\n";
    for (int i = 0; i < lines; ++i) {
        out += "- пункт " + QByteArray::number(i);
        if (i % 5 == step % 5) out += " с **правкой** номер " + QByteArray::number(step);
        out += "\n";
    }
    return out;
}

}  // namespace

static int ztRunSuite(int argc, char** argv) {
    const uint32_t seed = argc > 1 ? uint32_t(std::stoul(argv[1])) : 20260801u;
    std::fprintf(stderr, "зерно %u\n", seed);
    std::mt19937 rng(seed);

    QTemporaryDir dir;
    QDir().mkpath(QDir(dir.path()).filePath(QStringLiteral("history")));
    journal::History history(dir.path());

    const QString id = QStringLiteral("фаззинг");
    const QString path = history.pathFor(id);

    // Что мы САМИ считаем правдой: сколько записей и какие в них байты. Журнал
    // обязан сойтись с этим, иначе он рассказывает не ту историю.
    QVector<QByteArray> expected;   // слепок каждой записи; пусто — надгробие
    QVector<qint64> times;
    qint64 clock = 1'600'000'000'000LL;   // начало отсчёта; идёт только вперёд
    qint64 sizeBefore = 0;
    bool buried = false;

    const int rounds = 400;
    for (int round = 0; round < rounds && zt::g_failures == 0; ++round) {
        clock += 1000 + qint64(rng() % (7 * 24 * 3600 * 1000LL));
        const int what = int(rng() % 100);
        QString error;

        if (what < 8) {
            // Прореживание — единственное, что имеет право укорачивать журнал.
            const qint64 lastBefore = times.isEmpty() ? 0 : times.last();
            const qint64 now = clock + qint64(rng() % (30 * 24 * 3600 * 1000LL));
            const QVector<int> keep = journal::survivors(
                [&] {
                    journal::Journal j;
                    history.read(id, &j, &error);
                    return j.entries;
                }(),
                now);
            ZT_TRUE("прореживание проходит", history.thin(id, now, &error));

            // Ожидания подрезаем ровно так же, как обещает survivors.
            QVector<QByteArray> keptBytes;
            QVector<qint64> keptTimes;
            for (int i : keep) {
                keptBytes.append(expected[i]);
                keptTimes.append(times[i]);
            }
            if (!expected.isEmpty()) {
                expected = keptBytes;
                times = keptTimes;
                // Инвариант C: последняя запись не прореживается никогда — для
                // удалённой заметки это её вечный финальный слепок.
                ZT_EQ("последняя запись пережила прореживание", num(lastBefore),
                      num(times.isEmpty() ? 0 : times.last()));
            }

            // Идемпотентность: второй прогон при том же now не меняет ни байта.
            QFile f(path);
            const QByteArray before = f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
            f.close();
            ZT_TRUE("повторное прореживание проходит", history.thin(id, now, &error));
            QFile g(path);
            const QByteArray again = g.open(QIODevice::ReadOnly) ? g.readAll() : QByteArray();
            ZT_TRUE("и не меняет ни байта", before == again);
            sizeBefore = again.size();
        } else if (what < 12 && !expected.isEmpty() && !buried) {
            // Надгробие: заметку удалили. Слепка у записи нет.
            ZT_TRUE("надгробие пишется",
                    history.append(id, Kind::Tombstone, clock, QByteArray(), 0, &error));
            expected.append(QByteArray());
            times.append(clock);
            buried = true;
        } else if (what < 22 && !expected.isEmpty()) {
            // Восстановление: содержимое одной из прошлых записей возвращается
            // НОВОЙ записью. Журнал при этом только растёт — инвариант C.
            int from = int(rng() % uint32_t(expected.size()));
            while (from > 0 && expected[from].isEmpty()) --from;
            if (!expected[from].isEmpty()) {
                ZT_TRUE("восстановление пишется",
                        history.append(id, Kind::Restore, clock, expected[from], times[from],
                                       &error));
                expected.append(expected[from]);
                times.append(clock);
                buried = false;
            }
        } else {
            // Обычная правка: сохранение или внешнее изменение.
            const Kind kind = (rng() % 5) == 0 ? Kind::External : Kind::Save;
            const QByteArray body = noteAt(round, 20 + int(rng() % 400));
            ZT_TRUE("запись проходит", history.append(id, kind, clock, body, 0, &error));
            expected.append(body);
            times.append(clock);
            buried = false;
        }

        // --- инварианты после каждого шага ----------------------------------
        journal::Journal j;
        ZT_TRUE("журнал читается", history.read(id, &j, &error));
        ZT_TRUE("хвост цел", !j.tailTrimmed);
        ZT_EQ("записей столько, сколько мы написали", num(expected.size()),
              num(j.entries.size()));

        const qint64 size = QFileInfo(path).size();
        if (what >= 8) {
            // Инвариант B: не прореживали — файл обязан вырасти.
            ZT_TRUE("инвариант B: журнал только растёт", size > sizeBefore);
        }
        sizeBefore = size;

        // Каждый слепок собирается и сходится с отпечатком. Дорого, но это и
        // есть предмет проверки: цепочка поколений не имеет права разъехаться.
        for (int i = 0; i < j.entries.size() && zt::g_failures == 0; ++i) {
            ZT_EQ("время записи на месте", num(times[i]), num(j.entries[i].time));
            if (expected[i].isEmpty()) {
                ZT_TRUE("у надгробия слепка нет", !j.entries[i].hasSnapshot());
                continue;
            }
            QByteArray got;
            if (!history.snapshotAt(id, i, &got, &error)) {
                ZT_TRUE(("слепок №" + std::to_string(i) + " собирается: " + error.toStdString())
                            .c_str(),
                        false);
                break;
            }
            if (got != expected[i]) {
                ZT_EQ(("слепок №" + std::to_string(i) + " тот самый").c_str(),
                      num(expected[i].size()), num(got.size()));
                break;
            }
        }
    }

    if (zt::g_failures != 0)
        std::fprintf(stderr, "повторить: fuzz_journal_test %u\n", seed);
    return zt::report("фаззинг журнала");
}

// Набор целиком одним TEST: тело не тронуто, argv ему собран здесь.
// Дробить на отдельные проверки — отдельная работа, по одному набору.
TEST(FuzzJournal, All) {
    std::vector<QByteArray> ztArgs{QByteArrayLiteral("fuzz_journal_test")};
    std::vector<char*> ztArgv;
    for (QByteArray& a : ztArgs) ztArgv.push_back(a.data());
    EXPECT_EQ(0, ztRunSuite(int(ztArgv.size()), ztArgv.data()));
}
