// История правок: инвариант C этапа и правило «облик в историю не попадает».
//
// C. op + undo == identity на уровне IR, для каждой операции.

#include "document_builder.h"
#include "document_reader.h"
#include "edit_history.h"
#include "editor_ops.h"
#include "json_dump.h"
#include "parser.h"
#include "serializer.h"
#include "test_util.h"

#include <vector>
#include "testdata.h"

#include <QGuiApplication>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>

#include <string>

namespace {

using zametti::Document;

void check(bool ok, const std::string& what) {
    ++zt::g_checks;
    if (ok) return;
    ++zt::g_failures;
    std::printf("провал: %s\n", what.c_str());
}

void checkEqual(const std::string& expected, const std::string& actual,
                const std::string& what) {
    ++zt::g_checks;
    if (expected == actual) return;
    ++zt::g_failures;
    std::printf("провал: %s\n%s", what.c_str(), zt::diff(expected, actual).c_str());
}

Document parse(const char* source) { return zametti::parse(std::string(source)); }

// Смена оформления содержимого не касается. Это и есть правило: документ —
// содержимое, а не облик, поэтому облик в историю попадать не должен, а
// пересборка с другими настройками обязана дать тот же IR.
void checkAppearanceIsNotContent() {
    const char* const sources[] = {
        "# заголовок\n\nабзац с **жирным**\n",
        "- буллет\n- [ ] задача\n  - вложенный\n",
        "1. раз\n2. два\n",
        "```py\nx = 1\n```\n",
        "| a | b |\n|---|---|\n| 1 | 2 |\n",
    };
    for (const char* source : sources) {
        const Document ir = parse(source);

        QTextDocument small;
        zametti::buildDocument(ir, small, 1.0);
        QTextDocument large;
        zametti::buildDocument(ir, large, 2.5);

        checkEqual(zametti::toJson(zametti::readDocument(small)),
                   zametti::toJson(zametti::readDocument(large)),
                   std::string("масштаб изменил содержимое: ") + source);

        // И место курсора не должно зависеть от кегля: маркеры в текст не
        // входят, значит смещения одинаковы.
        check(small.characterCount() == large.characterCount(),
              std::string("масштаб изменил длину текста: ") + source);
    }
}

// Ход истории: шаг, отмена, повтор, обрубание ветки.
void checkHistoryOrder() {
    zametti::EditHistory history;
    history.reset(parse("раз\n"), 0);

    history.push(parse("раз\nдва\n"), 4);
    history.push(parse("раз\nдва\nтри\n"), 8);
    check(history.canUndo() && !history.canRedo(), "после двух шагов вперёд идти некуда");

    const zametti::HistoryStep* back = history.undo();
    check(back != nullptr && zametti::serialize(back->doc) == "раз\nдва\n",
          "undo возвращает предыдущее состояние");
    check(history.canRedo(), "после отмены можно вернуть");

    const zametti::HistoryStep* forward = history.redo();
    check(forward != nullptr && zametti::serialize(forward->doc) == "раз\nдва\nтри\n",
          "redo возвращает отменённое");

    // Новый шаг после отмены обрубает ветку: возвращать больше нечего.
    history.undo();
    history.push(parse("раз\nдругое\n"), 4);
    check(!history.canRedo(), "новый шаг обрубает отменённую ветку");

    // amend не заводит нового шага — так набор подряд остаётся одним шагом.
    const size_t before = history.size();
    history.amend(parse("раз\nдругое ещё\n"), 4);
    check(history.size() == before, "amend не заводит нового шага");
    check(zametti::serialize(history.current().doc) == "раз\nдругое ещё\n",
          "amend заменяет содержимое текущего шага");
}

// Ограничение длины: старые шаги уходят, текущий остаётся достижимым.
void checkHistoryLimit() {
    zametti::EditHistory history(4);
    history.reset(parse("ноль\n"), 0);
    for (int i = 1; i <= 10; ++i)
        history.push(parse((std::string("шаг ") + std::to_string(i) + "\n").c_str()), 0);

    check(history.size() == 4, "длина истории ограничена");
    check(zametti::serialize(history.current().doc) == "шаг 10\n",
          "текущий шаг после обрезки — последний");
    int steps = 0;
    while (history.undo() != nullptr) ++steps;
    check(steps == 3, "отменить можно ровно то, что осталось");
}

// Бюджет памяти держит глубину сверх счёта шагов: одна большая заметка
// укладывала в историю 200 своих копий — 72 МБ на замере заметки в 239 КБ.
void checkHistoryBudget() {
    // Шаг весит около килобайта; бюджета хватает на три.
    std::string big = "заметка\n\n";
    while (big.size() < 1000) big += "строка с текстом подлиннее\n";

    zametti::EditHistory history(200, 3 * 1024);
    history.reset(parse(big.c_str()), 0);
    for (int i = 1; i <= 20; ++i)
        history.push(parse((big + "правка " + std::to_string(i) + "\n").c_str()), 0);

    check(history.size() < 20, "бюджет обрезал историю раньше счёта шагов");
    check(history.bytes() <= 3 * 1024 || history.size() == 2,
          "вес истории уложился в бюджет");
    check(history.size() >= 2, "два шага остаются всегда: откатиться есть куда");
    check(zametti::serialize(history.current().doc) == big + "правка 20\n",
          "текущий шаг после обрезки по весу — последний");
    check(history.undo() != nullptr, "отмена после обрезки по весу работает");

    // Медианной заметке бюджет не мешает: глубина остаётся полной.
    zametti::EditHistory small(200, 32u * 1024 * 1024);
    small.reset(parse("мелочь\n"), 0);
    for (int i = 1; i <= 50; ++i)
        small.push(parse((std::string("мелочь ") + std::to_string(i) + "\n").c_str()), 0);
    check(small.size() == 51, "маленькой заметке бюджет глубину не режет");
}

// Инвариант C: операция и отмена возвращают ровно исходный IR.
void checkOpThenUndo(const char* source, const char* label) {
    const Document before = parse(source);

    QTextDocument doc;
    zametti::buildDocument(before, doc);

    zametti::EditHistory history;
    history.reset(before, 0);

    // Операция: правка текста плюс нормализующий проход — вместе один шаг.
    QTextCursor cursor(&doc);
    cursor.movePosition(QTextCursor::End);
    cursor.beginEditBlock();
    cursor.insertText(QStringLiteral(" хвост"));
    cursor.endEditBlock();
    zametti::syncLists(doc, {0, doc.blockCount() - 1});
    history.push(zametti::readDocument(doc), cursor.position());

    QString problem;
    check(zametti::listInvariantHolds(doc, &problem),
          std::string(label) + ": инвариант после операции — " + problem.toStdString());

    const zametti::HistoryStep* step = history.undo();
    check(step != nullptr, std::string(label) + ": отменять нечего");
    if (step == nullptr) return;

    // Отмена собирает документ заново — и он обязан прочитаться в исходный IR.
    QTextDocument restored;
    zametti::buildDocument(step->doc, restored);
    checkEqual(zametti::toJson(before), zametti::toJson(zametti::readDocument(restored)),
               std::string(label) + ": op + undo != identity");
}

const char* const kOpCases[] = {
    "абзац\n",
    "# заголовок\n",
    "- буллет\n- второй\n  - вложенный\n",
    "1. раз\n2. два\n",
    "- [ ] задача\n- [x] сделано\n",
    "> цитата\n",
    "```py\nx = 1\n```\n",
    "| a | b |\n|---|---|\n| 1 | 2 |\n",
};

}  // namespace

static int ztRunSuite(int argc, char** argv) {
    (void)argc;
    (void)argv;

    checkAppearanceIsNotContent();
    checkHistoryOrder();
    checkHistoryLimit();
    checkHistoryBudget();
    for (const char* source : kOpCases)
        checkOpThenUndo(source, (std::string("случай: ") + source).c_str());

    std::printf("проверок %d, провалов %d\n", zt::g_checks, zt::g_failures);
    return zt::g_failures == 0 ? 0 : 1;
}

// Набор целиком одним TEST: тело не тронуто, argv ему собран здесь.
// Дробить на отдельные проверки — отдельная работа, по одному набору.
TEST(Undo, All) {
    std::vector<QByteArray> ztArgs{QByteArrayLiteral("undo_test")};
    std::vector<char*> ztArgv;
    for (QByteArray& a : ztArgs) ztArgv.push_back(a.data());
    EXPECT_EQ(0, ztRunSuite(int(ztArgv.size()), ztArgv.data()));
}
