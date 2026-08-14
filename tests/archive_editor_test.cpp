// Архивация ОТКРЫТОЙ заметки: то, что делает человек, а не только хранилище.
//
// Набор появился по следу владельца: «попытка архивировать Typesetting Math
// каждый раз добавляет огромное количество символов \». Хранилищный набор
// (archive_test) при этом был зелёным, потому что спрашивал хранилище, а беда
// жила на стыке: заметку перед архивацией сохраняет РЕДАКТОР, а его круг
// «документ → IR» терял признак формулы, и сериализатор экранировал косые
// заново. Каждый заход удваивал их.
//
// Поэтому здесь проверяется весь путь целиком, ровно так, как он идёт в окне:
// открыли → сохранили → в архив → перечитали стаб → сохранили ещё раз →
// вернули из архива. И так три раза подряд: беда была не в первом заходе, а в
// накоплении.

#include "archive.h"
#include "editor_widget.h"
#include "journal.h"

#include "test_util.h"

#include <vector>
#include "testdata.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QTest>

#include <string>

namespace {

QString g_root;

QString notePath(const QString& id) { return g_root + QLatin1Char('/') + id + QStringLiteral(".md"); }

QString readFile(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    return QString::fromUtf8(f.readAll());
}

void writeFile(const QString& path, const QString& text) {
    QFile f(path);
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) f.write(text.toUtf8());
}

zametti::history::Rules rules() {
    zametti::history::Rules r;
    r.mergeChars = 100;
    r.mergeHours = 24;
    return r;
}

const char* kNote =
    "<!-- zametti\n"
    "id: 01formula0test\n"
    "created: 2026-01-01T00:00:00+03:00\n"
    "modified: 2026-01-02T00:00:00+03:00\n"
    "-->\n"
    "\n"
    "# Formulas\n"
    "\n"
    "Greek letters are typed using commands such as $\\gamma$ and $\\Gamma$.\n"
    "\n"
    "The integral $\\int_0^1 x^2 \\, dx$ has a thin space before $dx$.\n"
    "\n"
    "$$\\sum_{k=0}^\\infty \\frac{x^k}{k!} \\not= \\prod_{j=1}^{10} \\frac{j}{j+1}$$\n"
    "\n"
    "Roots: $\\sqrt{3}$, $\\sqrt[3]{12}$, $\\sqrt{1+\\sqrt{2}}$.\n";

}  // namespace

static int ztRunSuite(int argc, char** argv) {
    g_root = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QDir::tempPath();
    QDir(g_root).removeRecursively();
    QDir().mkpath(g_root + QStringLiteral("/.zametti"));
    QDir().mkpath(g_root + QStringLiteral("/history"));

    const QString id = QStringLiteral("01formula0test");
    const QString path = notePath(id);
    writeFile(path, QString::fromUtf8(kNote));

    zametti::NoteEditor editor;
    editor.resize(900, 700);
    editor.show();
    QTest::qWait(20);

    // Открытие КАНОНИЗИРУЕТ заметку и пишет её обратно, если она не канонична.
    // Именно поэтому беда была видна без единой правки: человек просто открывал
    // заметку, а файл менялся. Канон снимаем один раз и дальше сравниваем с ним.
    editor.openFile(path);
    QTest::qWait(60);
    const QString canon = readFile(path);
    ZT_TRUE("канон непуст", !canon.isEmpty());
    ZT_TRUE("формулы в каноне целы: \\gamma не удвоился",
            canon.contains(QStringLiteral("$\\gamma$")) &&
                !canon.contains(QStringLiteral("$\\\\gamma$")));
    ZT_TRUE("тонкий пробел на месте", canon.contains(QStringLiteral("x^2 \\, dx")));
    ZT_TRUE("индекс не экранирован", canon.contains(QStringLiteral("\\sum_{k=0}")));

    for (int round = 1; round <= 3; ++round) {
        const std::string tag = " (заход " + std::to_string(round) + ")";

        // 1. Открыли и сохранили — так делает окно перед архивацией.
        editor.openFile(path);
        QTest::qWait(40);
        editor.save(false);
        QTest::qWait(20);
        ZT_TRUE("открытие и запись файл не меняют" + tag, readFile(path) == canon);

        // 2. В архив.
        QString why;
        ZT_TRUE("архивация прошла" + tag,
                zametti::store::archiveNote(g_root, id, rules(), &why));
        const QString stub = readFile(path);
        ZT_TRUE("файл стал стабом" + tag,
                stub.contains(QStringLiteral("archived: yes")) &&
                    !stub.contains(QStringLiteral("\\gamma")));

        // 3. Перечитали стаб и сохранили — окно делает ровно это, а закрытие
        //    заметки зовёт запись принудительно.
        editor.openFile(path);
        QTest::qWait(40);
        editor.save(false, true);
        QTest::qWait(20);
        const QString afterSave = readFile(path);
        ZT_TRUE("пометка архива пережила запись" + tag,
                afterSave.contains(QStringLiteral("archived: yes")));
        ZT_TRUE("стаб не оброс телом" + tag, !afterSave.contains(QStringLiteral("\\gamma")));

        // 4. Вернули из архива — тело обязано вернуться БАЙТ В БАЙТ.
        ZT_TRUE("возврат прошёл" + tag, zametti::store::restoreNote(g_root, id, &why));
        const QString back = readFile(path);
        ZT_TRUE("тело вернулось побайтово" + tag, back == canon);
        if (back != canon) {
            // Место расхождения — в вывод: на заметке в килобайт «не совпало»
            // ничего не говорит.
            int i = 0;
            while (i < back.size() && i < canon.size() && back.at(i) == canon.at(i)) ++i;
            std::printf("  разошлось на %d: ждали [%s] вышло [%s]\n", i,
                        qPrintable(canon.mid(i, 40)), qPrintable(back.mid(i, 40)));
        }
    }

    // БИТУЮ ЗАМЕТКУ ТОЖЕ УБИРАЕМ. Правило владельца: пусть файл испорчен чем
    // угодно — правкой в чужом редакторе, сбоем, нашей же ошибкой, — раз шапка
    // на месте, архивация обязана пройти, и разбирать тело для этого не надо.
    {
        const QString brokenId = QStringLiteral("01broken00test");
        const QString brokenPath = notePath(brokenId);
        QString body = QStringLiteral(
            "<!-- zametti\nid: 01broken00test\ncreated: 2026-01-01T00:00:00+03:00\n"
            "modified: 2026-01-02T00:00:00+03:00\n-->\n\n# Битая\n\n");
        // Всё, чем markdown можно сломать: незакрытый забор, оборванная
        // таблица, гора косых, нулевой байт в тексте.
        body += QStringLiteral("```\nне закрыт забор\n\n| a | b\n|---\n");
        body += QString(2000, QLatin1Char('\\'));
        body += QStringLiteral("\n$$\\frac{a}{b\n\n<!-- не закрытый комментарий\n");
        writeFile(brokenPath, body);

        QString why;
        ZT_TRUE("битая заметка убирается в архив",
                zametti::store::archiveNote(g_root, brokenId, rules(), &why));
        const QString stub = readFile(brokenPath);
        ZT_TRUE("её стаб помечен архивным", stub.contains(QStringLiteral("archived: yes")));
        ZT_TRUE("и содержит заголовок", stub.contains(QStringLiteral("# Битая")));
        ZT_TRUE("и не содержит тела", !stub.contains(QStringLiteral("не закрыт забор")));

        // ТЕЛО ЦЕЛО ПОБАЙТОВО: в журнале лежит ровно то, что было в файле.
        zametti::journal::History history(g_root);
        zametti::journal::Journal read;
        QByteArray head;
        if (history.read(brokenId, &read, &why) && !read.entries.isEmpty())
            history.snapshotAt(brokenId, int(read.entries.size()) - 1, &head, &why);
        ZT_TRUE("тело битой заметки уехало в журнал байт в байт",
                QString::fromUtf8(head) == body);

        // И ВОЗВРАТ ОТДАЁТ ТЕ ЖЕ БАЙТЫ.
        ZT_TRUE("возврат битой прошёл", zametti::store::restoreNote(g_root, brokenId, &why));
        ZT_TRUE("битая вернулась байт в байт", readFile(brokenPath) == body);
    }

    return zt::report("архивация открытой заметки");
}

// Набор целиком одним TEST: тело не тронуто, argv ему собран здесь.
// Дробить на отдельные проверки — отдельная работа, по одному набору.
TEST(ArchiveEditor, All) {
    std::vector<QByteArray> ztArgs{QByteArrayLiteral("archive_editor_test")};
    ztArgs.push_back((zt::TestData::outDir(QStringLiteral("archive-editor"))).toLocal8Bit());
    std::vector<char*> ztArgv;
    for (QByteArray& a : ztArgs) ztArgv.push_back(a.data());
    EXPECT_EQ(0, ztRunSuite(int(ztArgv.size()), ztArgv.data()));
}
