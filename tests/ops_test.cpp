// Операции редактирования: IR до → операция → IR после.
//
// Документ собирается из IR, над ним работает операция, результат читается
// обратно и сравнивается по сериализованному виду — так расхождение читается
// глазами, а не по номерам полей.

#include "doc_model.h"
#include "document_builder.h"
#include "document_reader.h"
#include "editor_ops.h"
#include "marker.h"
#include "parser.h"
#include "serializer.h"
#include "test_util.h"

#include <QGuiApplication>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>

#include <string>
#include <vector>

namespace {

using zametti::Block;
using zametti::Document;
using zametti::Kind;

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

Block listItem(Kind kind, int level, const char* text) {
    Block b;
    b.kind = kind;
    b.level = level;
    b.text = text;
    return b;
}

Block paragraph(const char* text) {
    Block b;
    b.text = text;
    return b;
}

// Уровни, как они лежат в документе, — их и правит syncLists.
std::vector<int> levelsOf(const QTextDocument& doc) {
    std::vector<int> levels;
    for (QTextBlock block = doc.begin(); block.isValid(); block = block.next())
        levels.push_back(zametti::isListBlock(block) ? zametti::levelOf(block) : -1);
    return levels;
}

std::string levelsToString(const std::vector<int>& levels) {
    std::string out;
    for (int level : levels) {
        if (!out.empty()) out += ' ';
        out += level < 0 ? std::string(".") : std::to_string(level);
    }
    return out;
}

// syncLists правит уровни и не трогает текст.
void checkSync(const Document& before, const char* expectedLevels, const char* what) {
    QTextDocument doc;
    zametti::buildDocument(before, doc);
    zametti::syncLists(doc, {0, doc.blockCount() - 1});

    checkEqual(expectedLevels, levelsToString(levelsOf(doc)), what);

    QString problem;
    check(zametti::listInvariantHolds(doc, &problem),
          std::string(what) + ": инвариант нарушен — " + problem.toStdString());

    // Текст не должен пострадать: операция про уровни.
    Document after = zametti::readDocument(doc);
    check(after.size() == before.size(), std::string(what) + ": число блоков изменилось");
    for (size_t i = 0; i < after.size() && i < before.size(); ++i)
        check(after[i].text == before[i].text, std::string(what) + ": текст блока изменился");
}

void checkLevelNormalisation() {
    // Прыжок через уровень: подсписок не может быть глубже родителя больше чем
    // на единицу, иначе разбор файла даст не то, что мы показали.
    checkSync({listItem(Kind::Bullet, 0, "верх"), listItem(Kind::Bullet, 3, "провал")},
              "0 1", "прыжок 0→3 прижимается к 1");

    // Список, начинающийся с глубины: в файле такого не бывает.
    checkSync({listItem(Kind::Bullet, 2, "первый"), listItem(Kind::Bullet, 2, "второй")},
              "0 0", "прогон начинается с нулевого уровня");

    // Абзац рвёт прогон, и следующий список снова начинается с нуля.
    checkSync({listItem(Kind::Bullet, 0, "первый"), listItem(Kind::Bullet, 1, "вложенный"),
               paragraph("между"), listItem(Kind::Bullet, 2, "после абзаца")},
              "0 1 . 0", "абзац рвёт прогон");

    // Ступенька вниз разрешена любая: выйти можно сразу на верхний уровень.
    checkSync({listItem(Kind::Bullet, 0, "верх"), listItem(Kind::Bullet, 1, "глубже"),
               listItem(Kind::Bullet, 2, "ещё глубже"), listItem(Kind::Bullet, 0, "назад")},
              "0 1 2 0", "спуск на несколько уровней разрешён");

    // Уже верные уровни операция не трогает.
    checkSync({listItem(Kind::Ordered, 0, "раз"), listItem(Kind::Ordered, 1, "вложенный"),
               listItem(Kind::Ordered, 0, "два")},
              "0 1 0", "верные уровни остаются как были");
}

qreal marginOf(const QTextDocument& doc, int block) {
    return doc.findBlockByNumber(block).blockFormat().leftMargin();
}

// Колонку текста задаёт самый широкий маркер прогона: иначе под "10." текст
// начинался бы правее, чем под "1.", и левый край списка выходил бы рваным.
void checkGeometry() {
    Document doc;
    for (int i = 0; i < 12; ++i) doc.push_back(listItem(Kind::Ordered, 0, "пункт"));
    doc.push_back(listItem(Kind::Ordered, 1, "вложенный"));

    QTextDocument text;
    zametti::buildDocument(doc, text);

    const qreal column = marginOf(text, 0);
    bool aligned = true;
    for (int i = 1; i < 12; ++i) aligned = aligned && marginOf(text, i) == column;
    check(aligned, "весь прогон стоит в одной колонке, включая двузначные номера");

    const qreal nested = marginOf(text, 12);
    check(nested > column, "вложенный пункт стоит правее родителя");

    // Повторный проход ничего не меняет: операция идемпотентна.
    zametti::applyListGeometry(text, {0, text.blockCount() - 1});
    check(marginOf(text, 12) == nested, "повторный пересчёт геометрии ничего не меняет");

    // Буллеты и задачи — одно семейство, значит и один прогон: их текст стоит в
    // общей колонке, хотя рамка задачи шире кружка.
    QTextDocument mixed;
    zametti::buildDocument({listItem(Kind::Bullet, 0, "буллет"),
                            listItem(Kind::TaskUnchecked, 0, "задача"),
                            listItem(Kind::Bullet, 0, "снова буллет")},
                           mixed);
    check(marginOf(mixed, 0) == marginOf(mixed, 1) && marginOf(mixed, 1) == marginOf(mixed, 2),
          "буллеты и задачи одного прогона стоят в одной колонке");

    // Пересчёт по куску диапазона обязан дать то же, что по всему документу:
    // иначе правка одного пункта сдвигала бы колонку остальных.
    QTextDocument partial;
    zametti::buildDocument(doc, partial);
    zametti::applyListGeometry(partial, {5, 5});
    check(marginOf(partial, 0) == column && marginOf(partial, 11) == column,
          "пересчёт по одному блоку не сдвигает колонку прогона");
}

// Номер пункта считается двумя способами: обходом назад (для отрисовки) и
// прогоном вперёд (для геометрии). Расходиться они не имеют права.
void checkOrdinalAgreement(const std::string& source, const std::string& label) {
    QTextDocument doc;
    zametti::buildDocument(zametti::parse(source), doc);

    zametti::ListRuns runs;
    int number = 0;
    for (QTextBlock block = doc.begin(); block.isValid(); block = block.next(), ++number) {
        if (!zametti::isListBlock(block)) {
            runs.reset();
            continue;
        }
        const int forward =
            runs.next(zametti::levelOf(block), zametti::isOrdered(zametti::kindOf(block)));
        const int backward = zametti::ordinalOf(block);
        if (forward == backward) continue;
        ++zt::g_failures;
        std::printf("провал: %s, блок %d: вперёд %d, назад %d\n", label.c_str(), number,
                    forward, backward);
        return;
    }
    ++zt::g_checks;
}

const char* const kOrdinalCases[] = {
    "1. раз\n2. два\n3. три\n",
    "1. раз\n   1. вложенный\n   2. второй вложенный\n2. два\n",
    "- буллет\n- второй\n  - вложенный\n- третий\n",
    "1. раз\n2. два\n\nабзац\n\n1. снова раз\n",
    "- буллет\n1. номер\n- снова буллет\n",
    "- [ ] задача\n- буллет\n- [x] снова задача\n",
    "1. раз\n   - вложенный буллет\n2. два\n",
};

// Блоки-продолжения: признак относительный, он говорит про связь с предыдущим
// блоком. Операция способна эту связь порвать, и тогда документ обязан
// выправляться, а не молча превращаться в другой markdown.
void checkLiteralInvariant() {
    QTextDocument doc;
    zametti::buildDocument(zametti::parse("абзац\n\n```py\nодна\nдве\nтри\n```\n"), doc);

    QString problem;
    check(zametti::literalInvariantHolds(doc, &problem),
          "свежесобранный документ обязан быть в порядке: " + problem.toStdString());

    // Ломаем связь: убираем первую строку блока кода. Продолжение остаётся без
    // родителя того же рода.
    QTextCursor cursor(&doc);
    cursor.setPosition(doc.findBlockByNumber(1).position());
    cursor.movePosition(QTextCursor::NextBlock, QTextCursor::KeepAnchor);
    cursor.removeSelectedText();

    check(!zametti::literalInvariantHolds(doc),
          "разорванная связь обязана ловиться инвариантом");

    zametti::syncLiteralBlocks(doc, {0, doc.blockCount() - 1});
    check(zametti::literalInvariantHolds(doc, &problem),
          "после нормализации инвариант обязан держаться: " + problem.toStdString());

    // Признак снят, но содержимое не пострадало: строки просто стали двумя
    // блоками кода, а не одним. Ни байта не потеряно.
    const Document after = zametti::readDocument(doc);
    checkEqual("абзац\n\n```py\nдве\nтри\n```\n", zametti::serialize(after),
               "содержимое цело, язык блока не потерян");

    // Продолжение первым блоком документа быть не может.
    QTextDocument lone;
    zametti::buildDocument(zametti::parse("```\nодна\nдве\n```\n"), lone);
    QTextCursor head(&lone);
    head.setPosition(0);
    head.movePosition(QTextCursor::NextBlock, QTextCursor::KeepAnchor);
    head.removeSelectedText();
    check(!zametti::literalInvariantHolds(lone), "продолжение в начале документа — нарушение");
    zametti::syncLiteralBlocks(lone, {0, lone.blockCount() - 1});
    check(zametti::literalInvariantHolds(lone), "нормализация чинит и этот случай");
}

// Enter и Backspace: markdown до, место курсора, markdown после.
//
// Курсор задаётся номером блока документа и смещением в нём — так случай
// читается глазами, а не считается в уме от начала файла.
struct KeyCase {
    const char* before;
    int block;
    int offset;
    const char* after;
    const char* what;
};

void checkKey(bool (*op)(QTextDocument&, QTextCursor&), const KeyCase& c) {
    QTextDocument doc;
    zametti::buildDocument(zametti::parse(c.before), doc);

    QTextCursor cursor(&doc);
    const QTextBlock block = doc.findBlockByNumber(c.block);
    check(block.isValid(), std::string(c.what) + ": нет такого блока");
    if (!block.isValid()) return;
    cursor.setPosition(block.position() + c.offset);

    const bool handled = op(doc, cursor);
    const std::string actual = handled ? zametti::serialize(zametti::readDocument(doc))
                                       : std::string("<операция отказалась>");
    checkEqual(c.after, actual, c.what);

    if (!handled) return;
    QString problem;
    check(zametti::listInvariantHolds(doc, &problem),
          std::string(c.what) + ": инвариант списков — " + problem.toStdString());
    check(zametti::literalInvariantHolds(doc, &problem),
          std::string(c.what) + ": инвариант продолжений — " + problem.toStdString());
}

const KeyCase kEnterCases[] = {
    {"абзацхвост\n", 0, 5, "абзац\n\nхвост\n", "разрез в середине абзаца"},
    {"абзац\n", 0, 5, "абзац\n\n\n", "разрез в конце абзаца"},

    {"- пункт\n", 0, 5, "- пункт\n-\n", "новый пункт того же рода"},
    {"- пунктхвост\n", 0, 5, "- пункт\n- хвост\n", "разрез пункта посередине"},
    {"1. раз\n2. два\n", 1, 3, "1. раз\n2. два\n3.\n", "новый пункт нумерованного"},
    {"- верх\n  - вложенный\n", 1, 9, "- верх\n  - вложенный\n  -\n",
     "новый пункт того же уровня"},

    {"- [x] сделано\n", 0, 7, "- [x] сделано\n- [ ]\n", "новая задача невыполненная"},
    {"- [ ] дело\n", 0, 4, "- [ ] дело\n- [ ]\n", "невыполненная остаётся такой"},

    {"- пункт\n- \n", 1, 0, "- пункт\n\n\n", "пустой пункт снимает список"},
    {"- верх\n  - вложенный\n  - \n", 2, 0, "- верх\n  - вложенный\n\n\n",
     "пустой вложенный тоже"},

    {"# заголовокхвост\n", 0, 9, "# заголовок\n\nхвост\n", "разрез заголовка даёт абзац"},
    {"# заголовок\n", 0, 9, "# заголовок\n\n\n", "за заголовком идёт абзац"},

    {"```py\nодна\n```\n", 0, 4, "```py\nодна\n\n```\n", "Enter в коде даёт строку кода"},
    {"```py\nоднадве\n```\n", 0, 4, "```py\nодна\nдве\n```\n", "разрез строки кода"},
    {"```py\nодна\nдве\n```\n", 0, 4, "```py\nодна\n\nдве\n```\n",
     "разрез первой строки кода не рвёт блок"},

    {"> цитатахвост\n", 0, 6, "> цитата\n>\n> хвост\n", "разрез цитаты"},
};

const KeyCase kBackspaceCases[] = {
    {"- пункт\n", 0, 0, "пункт\n", "в начале пункта снимает список"},
    {"- верх\n  - вложенный\n", 1, 0, "- верх\n\nвложенный\n",
     "вложенный тоже становится абзацем"},
    {"- [x] дело\n", 0, 0, "дело\n", "задача становится абзацем"},
    {"1. раз\n2. два\n", 1, 0, "1. раз\n\nдва\n", "нумерованный пункт"},
    {"- пункт\n", 0, 3, "<операция отказалась>", "внутри текста — штатное поведение"},
    {"абзац\n", 0, 0, "<операция отказалась>", "в абзаце — штатное поведение"},
    {"```\nкод\n```\n", 0, 0, "<операция отказалась>", "в коде — штатное поведение"},
};

const KeyCase kIndentCases[] = {
    {"- раз\n- два\n", 1, 0, "- раз\n  - два\n", "второй пункт уходит под первый"},
    {"- раз\n- два\n", 0, 0, "<операция отказалась>", "первый пункт отступать некуда"},
    {"- раз\n  - два\n", 1, 0, "<операция отказалась>",
     "глубже родителя на единицу — уже некуда"},
    {"- раз\n  - два\n- три\n", 2, 0, "- раз\n  - два\n  - три\n",
     "под глубокого соседа можно на один уровень"},
    {"абзац\n", 0, 0, "<операция отказалась>", "вне списка Tab не при чём"},
    {"1. раз\n2. два\n", 1, 0, "1. раз\n   1. два\n", "нумерованный тоже"},
    {"- раз\n- два\n  - вложенный\n", 1, 0, "- раз\n  - два\n    - вложенный\n",
     "поддерево едет вместе с пунктом"},
};

const KeyCase kOutdentCases[] = {
    {"- раз\n  - два\n", 1, 0, "- раз\n- два\n", "пункт выходит на уровень выше"},
    {"- раз\n- два\n", 1, 0, "<операция отказалась>", "с верхнего уровня выходить некуда"},
    {"- раз\n  - два\n    - три\n", 1, 0, "- раз\n- два\n  - три\n",
     "поддерево выходит вместе с пунктом"},
    {"абзац\n", 0, 0, "<операция отказалась>", "вне списка Shift+Tab не при чём"},
};

const KeyCase kToggleCases[] = {
    {"- [ ] дело\n", 0, 0, "- [x] дело\n", "невыполненная становится выполненной"},
    {"- [x] дело\n", 0, 0, "- [ ] дело\n", "и обратно"},
    {"- буллет\n", 0, 0, "<операция отказалась>", "буллет не задача"},
    {"абзац\n", 0, 0, "<операция отказалась>", "абзац тем более"},
    {"1. раз\n", 0, 0, "<операция отказалась>", "нумерованный тоже не задача"},
};

// Выделение: несколько пунктов сразу.
struct RangeCase {
    const char* before;
    int firstBlock;
    int lastBlock;
    const char* after;
    const char* what;
};

void checkRange(bool (*op)(QTextDocument&, QTextCursor&), const RangeCase& c) {
    QTextDocument doc;
    zametti::buildDocument(zametti::parse(c.before), doc);

    QTextCursor cursor(&doc);
    cursor.setPosition(doc.findBlockByNumber(c.firstBlock).position());
    const QTextBlock last = doc.findBlockByNumber(c.lastBlock);
    cursor.setPosition(last.position() + last.length() - 1, QTextCursor::KeepAnchor);

    const bool handled = op(doc, cursor);
    const std::string actual = handled ? zametti::serialize(zametti::readDocument(doc))
                                       : std::string("<операция отказалась>");
    checkEqual(c.after, actual, c.what);

    if (!handled) return;
    QString problem;
    check(zametti::listInvariantHolds(doc, &problem),
          std::string(c.what) + ": инвариант списков — " + problem.toStdString());
}

const RangeCase kIndentRanges[] = {
    {"- раз\n- два\n- три\n", 1, 2, "- раз\n  - два\n  - три\n",
     "два выделенных пункта уходят вместе"},
    {"- раз\n- два\n- три\n", 0, 2, "<операция отказалась>",
     "с первым пунктом в выделении отступать некуда"},
};

const RangeCase kToggleRanges[] = {
    {"- [ ] раз\n- [ ] два\n", 0, 1, "- [x] раз\n- [x] два\n",
     "обе задачи отмечаются"},
    {"- [x] раз\n- [ ] два\n", 0, 1, "- [ ] раз\n- [ ] два\n",
     "смешанное выделение идёт за первой задачей"},
    {"- [ ] раз\n- буллет\n- [ ] два\n", 0, 2, "- [x] раз\n- буллет\n- [x] два\n",
     "буллет между задачами не трогается"},
};

// Перемещение пункта: markdown до, номер блока под курсором, куда двигаем.
struct MoveCase {
    const char* before;
    int block;
    int direction;
    const char* after;
    const char* what;
};

void checkMove(const MoveCase& c) {
    QTextDocument doc;
    zametti::buildDocument(zametti::parse(c.before), doc);

    QTextCursor cursor(&doc);
    cursor.setPosition(doc.findBlockByNumber(c.block).position());

    const zametti::MoveResult moved = zametti::moveListItem(doc, cursor, c.direction);
    const std::string actual =
        moved.done ? zametti::serialize(moved.doc) : std::string("<операция отказалась>");
    checkEqual(c.after, actual, c.what);
    if (!moved.done) return;

    // Пункт обязан оказаться там, куда указывает результат: иначе курсор уедет
    // в чужой пункт.
    QTextDocument rebuilt;
    zametti::buildDocument(moved.doc, rebuilt);
    const QTextBlock landed = zametti::blockForIrIndex(rebuilt, moved.irBlock);
    check(landed.isValid(), std::string(c.what) + ": курсор указывает в никуда");
    if (!landed.isValid()) return;
    checkEqual(doc.findBlockByNumber(c.block).text().toStdString(),
               landed.text().toStdString(),
               std::string(c.what) + ": курсор остался в том же пункте");
}

const MoveCase kMoveCases[] = {
    {"- раз\n- два\n- три\n", 1, -1, "- два\n- раз\n- три\n", "пункт вверх"},
    {"- раз\n- два\n- три\n", 1, 1, "- раз\n- три\n- два\n", "пункт вниз"},
    {"- раз\n- два\n", 0, -1, "<операция отказалась>", "первый вверх никуда"},
    {"- раз\n- два\n", 1, 1, "<операция отказалась>", "последний вниз никуда"},

    {"- раз\n  - вложенный\n- два\n", 0, 1, "- два\n- раз\n  - вложенный\n",
     "пункт едет вместе с поддеревом"},
    {"- раз\n- два\n  - вложенный\n", 0, 1, "- два\n  - вложенный\n- раз\n",
     "поддерево соседа тоже едет целиком"},
    {"- раз\n  - а\n  - б\n- два\n  - в\n", 0, 1,
     "- два\n  - в\n- раз\n  - а\n  - б\n", "меняются местами два поддерева"},

    {"- верх\n  - раз\n  - два\n", 2, -1, "- верх\n  - два\n  - раз\n",
     "вложенные меняются между собой"},
    {"- верх\n  - раз\n- другой\n", 1, 1, "<операция отказалась>",
     "у вложенного нет соседа того же уровня"},

    {"- раз\n\nабзац\n\n- два\n", 0, 1, "<операция отказалась>",
     "через абзац не прыгаем"},
    {"абзац\n", 0, 1, "<операция отказалась>", "вне списка не работает"},

    {"1. раз\n2. два\n", 0, 1, "1. два\n2. раз\n", "нумерованные перенумеровываются"},
    {"- [ ] дело\n- [x] сделано\n", 0, 1, "- [x] сделано\n- [ ] дело\n",
     "задачи сохраняют отметку"},
    {"- раз\n- два\n\n```\nкод\n```\n", 0, 1, "- два\n- раз\n\n```\nкод\n```\n",
     "блок кода рядом не мешает"},
};

// Курсор после разреза обязан оказаться в новом блоке: иначе набор продолжится
// не там, где человек его видит.
void checkCursorAfterSplit() {
    QTextDocument doc;
    zametti::buildDocument(zametti::parse("- пунктхвост\n"), doc);
    QTextCursor cursor(&doc);
    cursor.setPosition(doc.findBlockByNumber(0).position() + 5);
    zametti::splitBlockAtCursor(doc, cursor);
    check(cursor.blockNumber() == 1 && cursor.positionInBlock() == 0,
          "курсор после разреза стоит в начале нового блока");
}

}  // namespace

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);

    checkLevelNormalisation();
    checkGeometry();
    checkLiteralInvariant();
    for (const KeyCase& c : kEnterCases) checkKey(zametti::splitBlockAtCursor, c);
    for (const KeyCase& c : kBackspaceCases) checkKey(zametti::unwrapListItemAtCursor, c);
    for (const KeyCase& c : kIndentCases) checkKey(zametti::indentListItems, c);
    for (const KeyCase& c : kOutdentCases) checkKey(zametti::outdentListItems, c);
    for (const KeyCase& c : kToggleCases) checkKey(zametti::toggleTaskAtCursor, c);
    for (const RangeCase& c : kIndentRanges) checkRange(zametti::indentListItems, c);
    for (const RangeCase& c : kToggleRanges) checkRange(zametti::toggleTaskAtCursor, c);
    for (const MoveCase& c : kMoveCases) checkMove(c);
    checkCursorAfterSplit();
    for (const char* source : kOrdinalCases)
        checkOrdinalAgreement(source, std::string("номера: ") + source);

    std::printf("проверок %d, провалов %d\n", zt::g_checks, zt::g_failures);
    return zt::g_failures == 0 ? 0 : 1;
}
