// Разность двух версий: строки, блоки и заглушки.
//
// Что здесь проверяется и почему именно это:
//   - четыре случая, которые видит человек: дописали, стёрли, поправили,
//     переставили (последнее обязано выглядеть удалением плюс добавлением, а
//     не выдуманным родством непохожих строк);
//   - МЕТА В СРАВНЕНИЕ НЕ ИДЁТ: две записи одной и той же заметки отличаются
//     штампом modified всегда, и без этого правила всякий дифф начинался бы с
//     «изменена одна строка»;
//   - соответствие «строка ↔ блок»: каждая тронутая строка лежит в тронутом
//     блоке, и наоборот. На этом стоит обещание «два вида показывают одно и
//     то же»;
//   - заглушка «N строк удалено» встаёт перед тем блоком, который идёт следом.

#include "diff.h"
#include "pieces.h"

#include "test_util.h"

#include <vector>
#include "testdata.h"

#include <QString>
#include <QStringList>

#include <chrono>
#include <fstream>
#include <iterator>
#include <string>

using namespace zametti;

namespace {

template <typename T>
std::string num(T value) { return std::to_string(value); }

std::vector<Piece> parseOf(const std::string& text) { return pieces(text); }

// Тело заметки с шапкой: штамп modified у двух версий РАЗНЫЙ нарочно.
std::string note(const std::string& body, const char* stamp) {
    return std::string("<!-- zametti\ncreated: 2026-01-01T00:00:00Z\nmodified: ") + stamp +
           "\n-->\n\n" + body;
}

diff::Result compareNotes(const std::string& before, const std::string& after) {
    return diff::compare(diff::textOf(parseOf(before)).lines,
                         diff::textOf(parseOf(after)).lines);
}

// Одной строкой: сколько чего вышло.
std::string shapeOf(const diff::Result& result) {
    int same = 0, added = 0, removed = 0, changed = 0;
    for (const diff::Row& row : result.rows) switch (row.mark) {
        case diff::Mark::Same: ++same; break;
        case diff::Mark::Added: ++added; break;
        case diff::Mark::Removed: ++removed; break;
        case diff::Mark::Changed: ++changed; break;
    }
    return "= " + num(same) + ", + " + num(added) + ", - " + num(removed) + ", ~ " +
           num(changed);
}

// DTL НЕ ТРОГАЕТ ТО, ЧТО ЕМУ ДАЛИ. По коду видно (`Diff(const sequence& a,
// const sequence& b) : A(a), B(b)` — он берёт копии), но вопрос владельца
// прямой, и отвечать на него надо проверкой: сравниваемые массивы обязаны
// остаться теми же до последнего знака.
void checkCompareTouchesNothing() {
    const QStringList before = {QStringLiteral("раз"), QStringLiteral("два"),
                                QStringLiteral("три")};
    const QStringList after = {QStringLiteral("раз"), QStringLiteral("три"),
                               QStringLiteral("четыре")};
    const QStringList beforeCopy = before;
    const QStringList afterCopy = after;
    const diff::Result result = diff::compare(before, after);
    ZT_TRUE("сравнение что-то нашло", !result.identical());
    ZT_TRUE("первый массив не тронут", before == beforeCopy);
    ZT_TRUE("второй массив не тронут", after == afterCopy);
    // И обратный прогон по тем же данным — тоже.
    const diff::Result back = diff::compare(after, before);
    ZT_TRUE("после обратного прогона первый массив не тронут", before == beforeCopy);
    ZT_TRUE("и второй тоже", after == afterCopy);
    ZT_TRUE("обратное сравнение тоже что-то нашло", !back.identical());
}

void checkStampAlone() {
    // Одна и та же заметка, две записи. Различие только в штампе.
    const diff::Result result =
        compareNotes(note("# Заголовок\n\nТекст.\n", "2026-01-01T00:00:00Z"),
                     note("# Заголовок\n\nТекст.\n", "2026-01-02T03:04:05Z"));
    ZT_TRUE("штамп modified изменением не считается", result.identical());
    ZT_EQ("и строки все общие", std::string("= 3, + 0, - 0, ~ 0"), shapeOf(result));
}

void checkAdded() {
    const diff::Result result = compareNotes(note("# Заголовок\n\nОдин.\n", "a"),
                                             note("# Заголовок\n\nОдин.\n\nДва.\n", "b"));
    ZT_EQ("дописали абзац", std::string("= 3, + 2, - 0, ~ 0"), shapeOf(result));
}

void checkRemoved() {
    const diff::Result result = compareNotes(note("# Заголовок\n\nОдин.\n\nДва.\n", "a"),
                                             note("# Заголовок\n\nОдин.\n", "b"));
    ZT_EQ("стёрли абзац", std::string("= 3, + 0, - 2, ~ 0"), shapeOf(result));
}

void checkChanged() {
    const diff::Result result = compareNotes(note("# Заголовок\n\nОдин.\n", "a"),
                                             note("# Заголовок\n\nОдин с добавкой.\n", "b"));
    ZT_EQ("поправили строку", std::string("= 2, + 0, - 0, ~ 1"), shapeOf(result));
    for (const diff::Row& row : result.rows) {
        if (row.mark != diff::Mark::Changed) continue;
        ZT_EQ("и обе стороны при ней", std::string("Один."), row.textBefore.toStdString());
        ZT_EQ("и вторая тоже", std::string("Один с добавкой."), row.textAfter.toStdString());
    }
}

// СЧЁТ ПО СТОРОНАМ — как `git --numstat`: изменённая строка это одна убранная
// и одна добавленная. Баннер истории пишет «+m/−n», и числа обязаны сходиться
// с тем, что человек видит на поле знаками «+» и «−».
void checkSideCounts() {
    // Соотношение со строками результата — на трёх чистых случаях и на одном
    // смешанном, чью форму выбирает Майерс (какую именно — не наше дело).
    const auto agree = [](const std::string& what, const diff::Result& result) {
        int added = 0;
        int removed = 0;
        for (const diff::Row& row : result.rows) {
            if (row.mark == diff::Mark::Added || row.mark == diff::Mark::Changed) ++added;
            if (row.mark == diff::Mark::Removed || row.mark == diff::Mark::Changed) ++removed;
        }
        ZT_EQ(what + ": добавлено = добавленные + изменённые", num(added), num(result.added));
        ZT_EQ(what + ": убрано = убранные + изменённые", num(removed), num(result.removed));
    };
    const diff::Result add = compareNotes(note("# З\n\nОдин.\n", "a"),
                                          note("# З\n\nОдин.\n\nДва.\n", "b"));
    ZT_TRUE("дописали: +2/−0", add.added == 2 && add.removed == 0);
    agree("дописали", add);
    const diff::Result cut = compareNotes(note("# З\n\nОдин.\n\nДва.\n", "a"),
                                          note("# З\n\nОдин.\n", "b"));
    ZT_TRUE("стёрли: +0/−2", cut.added == 0 && cut.removed == 2);
    agree("стёрли", cut);
    const diff::Result fix = compareNotes(note("# З\n\nОдин.\n", "a"),
                                          note("# З\n\nОдин с добавкой.\n", "b"));
    ZT_TRUE("поправили строку: +1/−1", fix.added == 1 && fix.removed == 1);
    agree("поправили", fix);
    agree("смешанный",
          compareNotes(note("# З\n\nОдин.\n\nДва.\n\nТри.\n", "a"),
                       note("# З\n\nОдин с добавкой.\n\nТри.\n\nЧетыре.\n\nПять.\n", "b")));
    const diff::Result same = compareNotes(note("# З\n\nОдин.\n", "a"), note("# З\n\nОдин.\n", "b"));
    ZT_TRUE("у одинаковых — нули по обеим сторонам", same.added == 0 && same.removed == 0);
}

// ПЕРЕСТАНОВКА. Два абзаца поменяли местами; ждём удаление плюс добавление, а
// не «изменены обе строки»: строки-то не менялись, менялся их порядок, и
// выдумывать им родство было бы враньём.
void checkSwapIsDeleteAndAdd() {
    const diff::Result result =
        compareNotes(note("Первый абзац.\n\nВторой абзац.\n", "a"),
                     note("Второй абзац.\n\nПервый абзац.\n", "b"));
    int added = 0, removed = 0, changed = 0;
    for (const diff::Row& row : result.rows) {
        if (row.mark == diff::Mark::Added) ++added;
        if (row.mark == diff::Mark::Removed) ++removed;
        if (row.mark == diff::Mark::Changed) ++changed;
    }
    ZT_TRUE("перестановка — это удаление и добавление: + " + num(added) + ", - " +
                num(removed) + ", ~ " + num(changed),
            added >= 1 && removed >= 1 && changed == 0);
}

// СООТВЕТСТВИЕ «СТРОКА ↔ БЛОК». Тронутая строка обязана лежать в тронутом
// блоке, а тронутый блок — содержать хоть одну тронутую строку. Расхождение
// здесь означало бы, что вид с полосками и вид «как под капотом» показывают
// разное.
void checkBlocksAndLinesAgree() {
    const std::string before = note("# Заголовок\n\nОдин.\n\nДва.\n\n- пункт\n- пункт два\n", "a");
    const std::string after = note("# Заголовок\n\nОдин поправленный.\n\n- пункт\n"
                                   "- пункт два\n- пункт три\n", "b");
    const diff::Text shown = diff::textOf(parseOf(after));
    const diff::Result result = diff::compare(diff::textOf(parseOf(before)).lines, shown.lines);
    const diff::BlockMarks marks = diff::blockMarks(result, shown.blocks);

    ZT_EQ("блоков в разметке столько же, сколько в слепке", num(shown.blocks.size()),
          num(marks.blocks.size()));

    // Каждая тронутая строка — в тронутом блоке.
    bool everyLineInTouchedBlock = true;
    for (int line : result.changedAfterLines()) {
        int owner = -1;
        for (int i = 0; i < shown.blocks.size(); ++i)
            if (line >= shown.blocks[i].first && line < shown.blocks[i].first + shown.blocks[i].count)
                owner = i;
        everyLineInTouchedBlock = everyLineInTouchedBlock && owner >= 0 &&
                                  marks.blocks[owner] != diff::Mark::Same;
    }
    ZT_TRUE("каждая тронутая строка лежит в тронутом блоке", everyLineInTouchedBlock);

    // И наоборот: тронутый блок содержит хоть одну тронутую строку.
    const QVector<int> changed = result.changedAfterLines();
    bool everyBlockHasLine = true;
    for (int i = 0; i < marks.blocks.size(); ++i) {
        if (marks.blocks[i] == diff::Mark::Same) continue;
        bool any = false;
        for (int line : changed)
            any = any || (line >= shown.blocks[i].first &&
                          line < shown.blocks[i].first + shown.blocks[i].count);
        everyBlockHasLine = everyBlockHasLine && any;
    }
    ZT_TRUE("и в каждом тронутом блоке есть тронутая строка", everyBlockHasLine);

    // Удалённый абзац «Два.» — заглушка, и стоит она перед блоком, который
    // занял его место.
    int placeholders = 0;
    for (auto it = marks.gapBefore.constBegin(); it != marks.gapBefore.constEnd(); ++it)
        placeholders += it.value();
    ZT_TRUE("заглушка на удалённое есть: строк " + num(placeholders + marks.gapAtEnd),
            placeholders + marks.gapAtEnd > 0);
}

// Заглушка в хвосте: удалили конец заметки — вешать её не перед чем.
void checkRemovedTail() {
    const std::string before = note("# Заголовок\n\nОдин.\n\nХвост.\n", "a");
    const std::string after = note("# Заголовок\n\nОдин.\n", "b");
    const diff::Text shown = diff::textOf(parseOf(after));
    const diff::Result result = diff::compare(diff::textOf(parseOf(before)).lines, shown.lines);
    const diff::BlockMarks marks = diff::blockMarks(result, shown.blocks);
    ZT_EQ("удалённый хвост — заглушка в конце", num(2), num(marks.gapAtEnd));
    ZT_TRUE("и ни одного тронутого блока", [&] {
        for (diff::Mark m : marks.blocks)
            if (m != diff::Mark::Same) return false;
        return true;
    }());
}

// Карта блоков обязана покрывать весь текст: строк в блоках столько же,
// сколько строк в выводе. Иначе полоска на поле однажды встанет не туда, и
// заметит это не набор, а глаз владельца.
void checkMapCoversEverything() {
    const std::string body =
        "# Заголовок\n\nАбзац.\n\n- пункт\n- второй\n\n```\nкод\nв две строки\n```\n\n"
        "> цитата\n\n<!-- комментарий -->\n";
    const diff::Text text = diff::textOf(parseOf(note(body, "a")));
    int covered = 0;
    int last = 0;
    bool ordered = true;
    for (const BlockLines& b : text.blocks) {
        ordered = ordered && b.first >= last;
        last = b.first;
        covered += b.count;
    }
    ZT_TRUE("блоки идут по порядку", ordered);
    ZT_EQ("строки блоков покрывают весь текст", num(text.lines.size()), num(covered));
}

// --- прибор ------------------------------------------------------------------
//
// Замер, а не проверка: сколько стоит сравнение большой заметки. На нём стоит
// решение считать разность прямо при показе слепка, без потока и без кэша.
// Запускается руками:
//
//   taskset -c 0 ./diff_test --bench <файл.md>
//
// Рядом печатается эталонный счётный цикл: пока он стоит намертво, разброс —
// свойство измеряемого кода, а не машины.
long long yardstickMicros() {
    const auto start = std::chrono::steady_clock::now();
    volatile double sum = 0;
    for (int i = 0; i < 20'000'000; ++i) sum += double(i % 7);
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now() - start)
        .count();
}

void bench(const char* path) {
    std::ifstream file(path, std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(file)),
                           std::istreambuf_iterator<char>());
    if (text.empty()) {
        std::printf("не прочитан: %s\n", path);
        return;
    }
    // Вторая версия: правим каждую сотую строку и выбрасываем каждую двухсотую —
    // так выглядит день работы над заметкой, а не «поменялось всё».
    std::string changed;
    int line = 0;
    for (size_t at = 0; at < text.size();) {
        const size_t end = text.find('\n', at);
        const std::string one = text.substr(at, end == std::string::npos ? end : end - at);
        at = end == std::string::npos ? text.size() : end + 1;
        ++line;
        if (line % 200 == 0) continue;
        changed += one;
        if (line % 100 == 0) changed += " (правка)";
        changed += "\n";
    }

    std::printf("эталон до: %lld мкс\n", yardstickMicros());
    const std::vector<Piece> before = pieces(text);
    const std::vector<Piece> after = pieces(changed);
    long long best = -1;
    int rows = 0;
    for (int round = 0; round < 5; ++round) {
        const auto start = std::chrono::steady_clock::now();
        const diff::Text a = diff::textOf(before);
        const diff::Text b = diff::textOf(after);
        const diff::Result result = diff::compare(a.lines, b.lines);
        const diff::BlockMarks marks = diff::blockMarks(result, b.blocks);
        const long long spent = std::chrono::duration_cast<std::chrono::microseconds>(
                                    std::chrono::steady_clock::now() - start)
                                    .count();
        rows = int(result.rows.size());
        (void)marks;
        if (best < 0 || spent < best) best = spent;
    }
    std::printf("%s: строк %d, сравнение %lld мкс (минимум из пяти)\n", path, rows, best);
    std::printf("эталон после: %lld мкс\n", yardstickMicros());
}

}  // namespace

static int ztRunSuite(int argc, char** argv) {
    if (argc > 2 && std::string(argv[1]) == "--bench") {
        bench(argv[2]);
        return 0;
    }
    checkCompareTouchesNothing();
    checkStampAlone();
    checkAdded();
    checkRemoved();
    checkChanged();
    checkSideCounts();
    checkSwapIsDeleteAndAdd();
    checkBlocksAndLinesAgree();
    checkRemovedTail();
    checkMapCoversEverything();
    return zt::report("diff");
}

// Набор целиком одним TEST: тело не тронуто, argv ему собран здесь.
// Дробить на отдельные проверки — отдельная работа, по одному набору.
TEST(Diff, All) {
    std::vector<QByteArray> ztArgs{QByteArrayLiteral("diff_test")};
    std::vector<char*> ztArgv;
    for (QByteArray& a : ztArgs) ztArgv.push_back(a.data());
    EXPECT_EQ(0, ztRunSuite(int(ztArgv.size()), ztArgv.data()));
}
