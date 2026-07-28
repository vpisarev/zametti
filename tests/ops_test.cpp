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
using zametti::Marker;

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

Block listItem(Marker marker, int level, const char* text) {
    Block b;
    b.kind = Kind::ListItem;
    b.marker = marker;
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
    checkSync({listItem(Marker::Bullet, 0, "верх"), listItem(Marker::Bullet, 3, "провал")},
              "0 1", "прыжок 0→3 прижимается к 1");

    // Список, начинающийся с глубины: в файле такого не бывает.
    checkSync({listItem(Marker::Bullet, 2, "первый"), listItem(Marker::Bullet, 2, "второй")},
              "0 0", "прогон начинается с нулевого уровня");

    // Абзац рвёт прогон, и следующий список снова начинается с нуля.
    checkSync({listItem(Marker::Bullet, 0, "первый"), listItem(Marker::Bullet, 1, "вложенный"),
               paragraph("между"), listItem(Marker::Bullet, 2, "после абзаца")},
              "0 1 . 0", "абзац рвёт прогон");

    // Ступенька вниз разрешена любая: выйти можно сразу на верхний уровень.
    checkSync({listItem(Marker::Bullet, 0, "верх"), listItem(Marker::Bullet, 1, "глубже"),
               listItem(Marker::Bullet, 2, "ещё глубже"), listItem(Marker::Bullet, 0, "назад")},
              "0 1 2 0", "спуск на несколько уровней разрешён");

    // Уже верные уровни операция не трогает.
    checkSync({listItem(Marker::Ordered, 0, "раз"), listItem(Marker::Ordered, 1, "вложенный"),
               listItem(Marker::Ordered, 0, "два")},
              "0 1 0", "верные уровни остаются как были");
}

qreal marginOf(const QTextDocument& doc, int block) {
    return doc.findBlockByNumber(block).blockFormat().leftMargin();
}

// Колонку текста задаёт самый широкий маркер прогона: иначе под "10." текст
// начинался бы правее, чем под "1.", и левый край списка выходил бы рваным.
void checkGeometry() {
    Document doc;
    for (int i = 0; i < 12; ++i) doc.push_back(listItem(Marker::Ordered, 0, "пункт"));
    doc.push_back(listItem(Marker::Ordered, 1, "вложенный"));

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

    // Буллеты и задачи для нумерации одно семейство, а вот колонка у каждого
    // своя. Общая колонка означала бы, что кружок стоит по ширине чекбокса — и
    // прыгает, стоит отцепить буллеты от задач. Ширина кружка от соседей
    // зависеть не должна.
    QTextDocument mixed;
    zametti::buildDocument({listItem(Marker::Bullet, 0, "буллет"),
                            listItem(Marker::Task, 0, "задача"),
                            listItem(Marker::Bullet, 0, "снова буллет")},
                           mixed);
    check(marginOf(mixed, 0) < marginOf(mixed, 1),
          "кружок не равняется по ширине чекбокса");
    check(marginOf(mixed, 0) == marginOf(mixed, 2),
          "оба кружка стоят одинаково, задача между ними им не мешает");

    // И тот же кружок сам по себе стоит там же, где рядом с задачами.
    QTextDocument alone;
    zametti::buildDocument({listItem(Marker::Bullet, 0, "буллет")}, alone);
    check(marginOf(alone, 0) == marginOf(mixed, 0),
          "отцепив буллет от задач, кружок никуда не прыгает");

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
            // Ни пустая строка, ни блок внутри пункта список не заканчивают:
            // нумерация за ними продолжается. Так же смотрят и отрисовка, и
            // сериализатор.
            if (zametti::isVSpaceBlock(block) || zametti::levelOf(block) >= 0) continue;
            runs.reset();
            continue;
        }
        const int forward =
            runs.next(zametti::levelOf(block), zametti::isOrderedBlock(block));
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
    // Нумерацию не сбивает ничто, что стоит внутри пункта: ни второй абзац,
    // ни блок кода. Через сериализацию это не видно — номера рисует отрисовка.
    "1. раз\n\n   продолжение\n\n2. два\n",
    "1. раз\n\n   ```\n   код\n   ```\n\n2. два\n",
    "- раз\n\n  ```\n  код\n  ```\n\n- два\n",
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
    // Блок 1 — пустая строка, блок кода начинается со второго.
    cursor.setPosition(doc.findBlockByNumber(2).position());
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
    // Обычный текст: Enter переносит строку внутри абзаца, а не заводит новый.
    // Заметки пишут так, а отбивка между абзацами остаётся для настоящих
    // абзацев.
    {"абзацхвост\n", 0, 5, "абзац\nхвост\n", "перенос строки внутри абзаца"},
    {"абзац\nхвост\n", 0, 6, "абзац\n\nхвост\n",
     "второй Enter подряд разрезает абзац"},
    {"> цитатахвост\n", 0, 6, "> цитата\n> хвост\n", "перенос внутри цитаты"},
    // Разрезает только Enter на ПУСТОЙ строке — пустой целиком, и до курсора, и
    // после. В начале строки с текстом Enter переносит строку: иначе десять
    // нажатий давали бы пять пустых строк вместо десяти.
    {"абзац\nхвост\n", 0, 6, "абзац\n\nхвост\n",
     "в начале строки с текстом Enter переносит строку"},
    // Разрез на пустой строке проверяется живым набором в list_test: пустую
    // строку внутри блока markdown не выражает, и подать её сюда исходником
    // нельзя — разбор вернёт два блока.

    {"- пункт\n", 0, 5, "- пункт\n-\n", "новый пункт того же рода"},

    // Enter в начале пункта заводит пустой пункт НАД текущим — так вставляют
    // пункт между двумя. Курсор остаётся в новом пустом пункте: печатать нужно
    // именно там.
    {"- раз\n- два\n", 1, 0, "- раз\n-\n- два\n", "Enter в начале заводит пункт выше"},
    {"- раз\n- два\n", 0, 0, "-\n- раз\n- два\n", "и в первом пункте тоже"},
    {"- пунктхвост\n", 0, 5, "- пункт\n- хвост\n", "разрез пункта посередине"},
    {"1. раз\n2. два\n", 1, 3, "1. раз\n2. два\n3.\n", "новый пункт нумерованного"},
    {"- верх\n  - вложенный\n", 1, 9, "- верх\n  - вложенный\n  -\n",
     "новый пункт того же уровня"},

    {"- [x] сделано\n", 0, 7, "- [x] сделано\n- [ ]\n", "новая задача невыполненная"},
    {"- [ ] дело\n", 0, 4, "- [ ] дело\n- [ ]\n", "невыполненная остаётся такой"},

    {"- пункт\n- \n", 1, 0, "- пункт\n\n\n", "пустой пункт снимает список"},
    {"- верх\n  - вложенный\n  - \n", 2, 0, "- верх\n  - вложенный\n\n\n",
     "пустой вложенный тоже"},

    // Пустой строки между заголовком и абзацем markdown не требует, а сами мы
    // её не ставим: сколько их набрали, столько и будет.
    {"# заголовокхвост\n", 0, 9, "# заголовок\nхвост\n", "разрез заголовка даёт абзац"},
    {"# заголовок\n", 0, 9, "# заголовок\n\n", "за заголовком идёт абзац"},

    {"```py\nодна\n```\n", 0, 4, "```py\nодна\n\n```\n", "Enter в коде даёт строку кода"},

    // Заведение и закрытие блока кода — так же, как это пишут в файле.
    {"\\`\\`\\`\n", 0, 3, "```\n```\n", "три кавычки и Enter заводят блок кода"},
    {"\\`\\`\\`py\n", 0, 5, "```py\n```\n", "язык за забором переезжает в блок"},
    {"\\~\\~\\~\n", 0, 3, "```\n```\n", "тильды тоже заводят блок"},
    // Пустая строка из блока не выводит: в длинном коде они разделяют
    // логические части, и вылетать из блока на каждой было бы мучением.
    {"```\nкод\n\n```\n", 1, 0, "```\nкод\n\n\n```\n",
     "пустая строка остаётся содержимым блока"},
    {"```py\nоднадве\n```\n", 0, 4, "```py\nодна\nдве\n```\n", "разрез строки кода"},
    {"```py\nодна\nдве\n```\n", 0, 4, "```py\nодна\n\nдве\n```\n",
     "разрез первой строки кода не рвёт блок"},

};

const KeyCase kBackspaceCases[] = {
    // Первый пункт сливать не с чем — он просто перестаёт быть пунктом.
    {"- пункт\n", 0, 0, "пункт\n", "в начале первого пункта снимает список"},
    {"- [x] дело\n", 0, 0, "дело\n", "задача становится абзацем"},
    // А если предыдущий пункт есть, Backspace сливает с ним — так он ведёт себя
    // всюду, и это привычнее превращения пункта в абзац на месте.
    {"- верх\n- низ\n", 1, 0, "- верхниз\n", "пункт сливается с предыдущим"},
    {"1. раз\n2. два\n", 1, 0, "1. раздва\n", "нумерованный сливается тоже"},
    // С чужим списком не сливаемся: у родителя другой уровень. Иначе буллет
    // притягивало бы к вложенной задаче, строение рушилось молча, а человек
    // всего лишь снимал маркер.
    {"- верх\n  - вложенный\n", 1, 0, "- верх\n\nвложенный\n",
     "вложенный не сливается с родителем, а теряет маркер"},
    {"- буллет\n- [ ] задача\n", 1, 0, "- буллет\n\nзадача\n",
     "задача не сливается с буллетом"},
    {"1. номер\n- буллет\n", 1, 0, "1. номер\n\nбуллет\n",
     "буллет не сливается с нумерованным"},
    {"- пустой\n- \n", 1, 0, "- пустой\n", "пустой пункт просто исчезает"},
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

const KeyCase kBulletCases[] = {
    {"абзац\n", 0, 0, "- абзац\n", "абзац становится буллетом"},
    {"1. раз\n", 0, 0, "- раз\n", "нумерованный становится буллетом"},
    {"- [x] дело\n", 0, 0, "- дело\n", "задача теряет отметку, раз просили буллет"},
    {"# заголовок\n", 0, 0, "- заголовок\n", "заголовок становится буллетом"},
    {"- буллет\n", 0, 0, "- буллет\n", "буллет остаётся собой"},
    {"1. [x] дело\n", 0, 0, "- [x] дело\n",
     "содержимое с отметкой становится задачей: так его прочтёт файл"},
    {"```\nкод\n```\n", 0, 0, "<операция отказалась>", "блок кода не трогается"},
};

const KeyCase kOrderedCases[] = {
    {"- буллет\n", 0, 0, "1. буллет\n", "буллет становится нумерованным"},
    // Отметка задачи просто исчезает. Переезд её в текст выглядел как
    // "1. [x] дело" — то есть как ошибка, а не как забота о содержимом.
    {"- [x] дело\n", 0, 0, "1. дело\n", "отметка выполненной задачи исчезает"},
    {"- [ ] дело\n", 0, 0, "1. дело\n", "и невыполненной тоже"},
    {"абзац\n", 0, 0, "1. абзац\n", "абзац становится нумерованным"},
};

const KeyCase kTaskCases[] = {
    {"- буллет\n", 0, 0, "- [ ] буллет\n", "буллет становится задачей"},
    {"абзац\n", 0, 0, "- [ ] абзац\n", "абзац становится задачей"},
    {"- [x] дело\n", 0, 0, "- [ ] дело\n", "выполненная сбрасывается: род задан, не переключён"},
};

const KeyCase kParagraphCases[] = {
    {"- буллет\n", 0, 0, "буллет\n", "буллет становится абзацем"},
    {"  - вложенный\n", 0, 0, "вложенный\n", "уровень снимается тоже"},
    {"# заголовок\n", 0, 0, "заголовок\n", "заголовок становится абзацем"},
    {"> цитата\n", 0, 0, "цитата\n", "цитата тоже"},
    {"- [x] дело\n", 0, 0, "дело\n", "задача теряет отметку"},
};

const RangeCase kOrderedRanges[] = {
    {"- раз\n  - вложенный\n", 0, 1, "1. раз\n   1. вложенный\n",
     "уровни при смене рода сохраняются"},
};

const RangeCase kConvertRanges[] = {
    // Пустые строки обязательны: без них "абзац" стал бы ленивым продолжением
    // пункта, а не отдельным блоком.
    {"- буллет\n\n1. номер\n\nабзац\n", 0, 4, "- буллет\n\n- номер\n\n- абзац\n",
     "смешанное выделение приводится целиком"},

};

// Закрывающая кавычка делает встроенный код. Набранное подаётся целиком, а
// курсор ставится за последней кавычкой — как после её набора.
struct CodeSpanCase {
    const char* before;
    const char* typed;
    const char* after;
    const char* what;
};

void checkCodeSpan(const CodeSpanCase& c) {
    QTextDocument doc;
    zametti::buildDocument(zametti::parse(c.before), doc);

    QTextCursor typing(&doc);
    typing.movePosition(QTextCursor::End);
    typing.insertText(QString::fromUtf8(c.typed));

    QTextCursor cursor(&doc);
    cursor.movePosition(QTextCursor::End);
    const bool handled = zametti::applyCodeSpanRuleAtCursor(doc, cursor);
    const std::string actual = handled ? zametti::serialize(zametti::readDocument(doc))
                                       : std::string("<правило не сработало>");
    checkEqual(c.after, actual, c.what);
}

const CodeSpanCase kCodeSpanCases[] = {
    {"вот\n", " `код`", "вот `код`\n", "кавычки делают встроенный код"},
    {"вот\n", " ``", "<правило не сработало>", "пусто между кавычками — не код"},
    {"вот\n", " `", "<правило не сработало>", "одна кавычка ничего не делает"},
    {"вот\n", " код`", "<правило не сработало>", "без открывающей кавычки тоже"},
    {"```\nкод\n```\n", " `x`", "<правило не сработало>", "в блоке кода правила нет"},
};

// Выделенное в блок кода и обратно. Выделение задаётся номерами блоков.
struct CodeBlockCase {
    const char* source;
    int firstBlock;
    int lastBlock;
    const char* after;
    const char* what;
};

void checkCodeBlock(const CodeBlockCase& c) {
    QTextDocument doc;
    zametti::buildDocument(zametti::parse(c.source), doc);

    QTextCursor cursor(&doc);
    cursor.setPosition(doc.findBlockByNumber(c.firstBlock).position());
    const QTextBlock last = doc.findBlockByNumber(c.lastBlock);
    cursor.setPosition(last.position() + last.length() - 1, QTextCursor::KeepAnchor);

    const zametti::MoveResult result = zametti::toggleCodeBlock(doc, cursor);
    checkEqual(c.after,
               result.done ? zametti::serialize(result.doc)
                           : std::string("<операция отказалась>"),
               c.what);
}

// Выделение частью блока: абзац с мягкими переносами — один блок, а строк в
// нём много. В блок кода должно уйти только выделенное.
struct PartialCodeCase {
    const char* source;
    int from;
    int to;
    const char* after;
    const char* what;
};

void checkPartialCodeBlock(const PartialCodeCase& c) {
    QTextDocument doc;
    zametti::buildDocument(zametti::parse(c.source), doc);

    QTextCursor cursor(&doc);
    cursor.setPosition(doc.firstBlock().position() + c.from);
    cursor.setPosition(doc.firstBlock().position() + c.to, QTextCursor::KeepAnchor);

    const zametti::MoveResult result = zametti::toggleCodeBlock(doc, cursor);
    checkEqual(c.after,
               result.done ? zametti::serialize(result.doc)
                           : std::string("<операция отказалась>"),
               c.what);
}

// "первая\nвторая\nтретья" — по семь знаков на строку с разделителем.
const PartialCodeCase kPartialCodeCases[] = {
    {"первая\nвторая\nтретья\n", 7, 13, "первая\n```\nвторая\n```\nтретья\n",
     "средняя строка уходит в код одна"},
    {"первая\nвторая\nтретья\n", 0, 6, "```\nпервая\n```\nвторая\nтретья\n",
     "первая строка — и остальные остаются абзацем"},
    {"первая\nвторая\nтретья\n", 14, 20, "первая\nвторая\n```\nтретья\n```\n",
     "последняя строка"},
    // Конец выделения ровно на начале строки: её человек не выделял.
    {"первая\nвторая\nтретья\n", 7, 14, "первая\n```\nвторая\n```\nтретья\n",
     "строка, начатая на границе, в код не идёт"},
    // Полстроки нельзя: границы притягиваются к краям строк.
    {"первая\nвторая\nтретья\n", 9, 11, "первая\n```\nвторая\n```\nтретья\n",
     "выделение внутри строки берёт строку целиком"},
};

const CodeBlockCase kCodeBlockCases[] = {
    {"абзац\n", 0, 0, "```\nабзац\n```\n", "абзац становится блоком кода"},
    {"раз\n\nдва\n", 0, 2, "```\nраз\n\nдва\n```\n",
     "два абзаца сливаются в один блок"},
    {"```\nкод\n```\n", 0, 0, "код\n", "блок кода возвращается в текст"},
    {"```\nраз\nдва\n```\n", 0, 1, "раз\nдва\n",
     "многострочный код становится одним абзацем"},
    // Отступы внутри кода при обратном ходе остаются: в файл они уйдут
    // неразрывными пробелами, как и всякий отступ вне кода.
    {"```\nif x:\n    y\n```\n", 0, 1, "if x:\n    y\n",
     "отступ кода переживает возврат в текст"},
    {"- пункт\n", 0, 0, "```\nпункт\n```\n", "пункт списка тоже можно"},
};

// Фигура буллета зависит от уровня вложенности: так вложенность видна сразу.
// Последняя фигура достаётся всем уровням глубже — иначе на пятом уровне буллет
// пропал бы вовсе.
void checkBulletShapes() {
    const std::vector<zametti::BulletShape> saved = zametti::appearance().bulletShapes;

    zametti::appearance().bulletShapes = {zametti::BulletShape::Disc,
                                                 zametti::BulletShape::Circle,
                                                 zametti::BulletShape::Square};
    check(zametti::bulletShapeFor(0) == zametti::BulletShape::Disc,
          "первый уровень — сплошной кружок");
    check(zametti::bulletShapeFor(1) == zametti::BulletShape::Circle,
          "второй — незаполненный");
    check(zametti::bulletShapeFor(2) == zametti::BulletShape::Square, "третий — квадратик");
    check(zametti::bulletShapeFor(7) == zametti::BulletShape::Square,
          "глубже — та же последняя фигура");
    check(zametti::bulletShapeFor(-1) == zametti::BulletShape::Disc,
          "уровень ниже нуля не роняет выбор");

    // Пустой список фигур оставаться без буллетов не должен.
    zametti::appearance().bulletShapes.clear();
    check(zametti::bulletShapeFor(0) == zametti::BulletShape::Disc,
          "без настроенных фигур остаётся сплошной кружок");

    zametti::appearance().bulletShapes = saved;
}

// Номер блока IR и обратный переход. Соответствие не один к одному: литеральный
// блок лежит построчно, и каждая его строка обязана указывать на СВОЙ блок IR.
// Пока это считалось по предыдущим блокам, строка-продолжение получала номер
// следующего блока — и всякая правка над IR била мимо.
void checkIrIndex(const char* source) {
    QTextDocument doc;
    const zametti::Document ir = zametti::parse(source);
    zametti::buildDocument(ir, doc);

    int expected = -1;
    for (QTextBlock block = doc.begin(); block.isValid(); block = block.next()) {
        if (!zametti::isContinuationBlock(block)) ++expected;
        const int actual = zametti::irIndexOfBlock(block);
        checkEqual(std::to_string(expected), std::to_string(actual),
                   std::string("номер блока IR для документа ") + source);
        // И обратный переход обязан вести к началу того же логического блока.
        const QTextBlock back = zametti::blockForIrIndex(doc, actual);
        check(back.isValid() && back.blockNumber() <= block.blockNumber(),
              "обратный переход ведёт к своему блоку");
    }
    checkEqual(std::to_string(int(ir.size()) - 1), std::to_string(expected),
               std::string("блоков IR столько же, сколько насчитали: ") + source);
}

// Автозамена: набранное в начале блока, положение курсора после пробела.
struct RuleCase {
    const char* typed;     // что оказалось в блоке к моменту проверки
    const char* before;    // документ до
    const char* after;
    const char* what;
};

void checkRule(const RuleCase& c) {
    QTextDocument doc;
    zametti::buildDocument(zametti::parse(c.before), doc);

    // Набираем в начало первого блока — ровно так, как это делает человек.
    QTextCursor typing(&doc);
    typing.setPosition(doc.firstBlock().position());
    typing.insertText(QString::fromUtf8(c.typed));

    QTextCursor cursor(&doc);
    cursor.setPosition(doc.firstBlock().position() + int(QString::fromUtf8(c.typed).size()));

    const bool handled = zametti::applyInputRuleAtCursor(doc, cursor);
    const std::string actual = handled ? zametti::serialize(zametti::readDocument(doc))
                                       : std::string("<правило не сработало>");
    checkEqual(c.after, actual, c.what);
}

const RuleCase kRuleCases[] = {
    {"- ", "текст\n", "- текст\n", "дефис делает буллет"},
    {"* ", "текст\n", "- текст\n", "звёздочка тоже, но в файл идёт дефис"},
    {"+ ", "текст\n", "- текст\n", "и плюс"},

    {"1. ", "текст\n", "1. текст\n", "номер с точкой"},
    {"1) ", "текст\n", "1. текст\n", "номер со скобкой"},
    {"42. ", "текст\n", "1. текст\n", "многозначный номер"},

    {"# ", "текст\n", "# текст\n", "одна решётка — заголовок"},
    {"### ", "текст\n", "### текст\n", "три решётки — третий уровень"},
    {"###### ", "текст\n", "###### текст\n", "шесть — шестой"},
    {"####### ", "текст\n", "<правило не сработало>", "семь решёток заголовком не делают"},

    // Задача одним махом, без промежуточной автозамены на буллет: до закрывающей
    // скобки дело часто не доходит, а намерение уже понятно.
    {"-[ ", "текст\n", "- [ ] текст\n", "дефис со скобкой сразу дают задачу"},
    {"-[] ", "текст\n", "- [ ] текст\n", "с закрытой скобкой тоже"},
    {"-[ ] ", "текст\n", "- [ ] текст\n", "и с пробелом внутри"},
    {"-[x] ", "текст\n", "- [x] текст\n", "отмеченная задача сразу"},
    {"-[X] ", "текст\n", "- [x] текст\n", "заглавная тоже"},
    {"- [ ", "текст\n", "<правило не сработало>",
     "с пробелом после маркера недописанная скобка не в счёт"},
    {"*[ ", "текст\n", "- [ ] текст\n", "звёздочка со скобкой"},
    {"+[x] ", "текст\n", "- [x] текст\n", "плюс со скобкой"},
    {"-[y] ", "текст\n", "<правило не сработало>", "чужая буква задачей не делает"},

    // Длинный путь через две автозамены остаётся: буллет, потом скобки.
    {"[ ] ", "- пункт\n", "- [ ] пункт\n", "скобки в буллете делают задачу"},
    {"[ ", "- пункт\n", "<правило не сработало>",
     "в готовом буллете недописанная скобка ждёт закрывающей"},
    {"[] ", "- пункт\n", "- [ ] пункт\n", "и закрытая без пробела"},
    {"[x] ", "- пункт\n", "- [x] пункт\n", "и отмеченную"},
    {"[ ] ", "текст\n", "<правило не сработало>", "в абзаце скобки ничего не делают"},
    {"[ ] ", "1. пункт\n", "<правило не сработало>",
     "в нумерованном пункте скобки — обычный текст"},

    {"- ", "- пункт\n", "<правило не сработало>", "список списком уже не сделаешь"},
    {"-", "текст\n", "<правило не сработало>", "без пробела правило молчит"},
    {"обычный ", "текст\n", "<правило не сработало>", "обычные слова не задевают"},
    {"- ", "```\nкод\n```\n", "<правило не сработало>", "в блоке кода правил нет"},
};

// Начертание на выделение: markdown до, границы выделения в блоке, markdown после.
struct StyleCase {
    const char* before;
    int block;
    int from;
    int to;
    const char* after;
    const char* what;
};

void checkStyle(bool (*op)(QTextDocument&, QTextCursor&), const StyleCase& c) {
    QTextDocument doc;
    zametti::buildDocument(zametti::parse(c.before), doc);

    QTextCursor cursor(&doc);
    const QTextBlock block = doc.findBlockByNumber(c.block);
    cursor.setPosition(block.position() + c.from);
    if (c.to > c.from)
        cursor.setPosition(block.position() + c.to, QTextCursor::KeepAnchor);

    const bool handled = op(doc, cursor);
    const std::string actual = handled ? zametti::serialize(zametti::readDocument(doc))
                                       : std::string("<операция отказалась>");
    checkEqual(c.after, actual, c.what);
}

const StyleCase kBoldCases[] = {
    {"обычный текст\n", 0, 0, 7, "**обычный** текст\n", "жирным становится выделенное"},
    {"**жирный** текст\n", 0, 0, 6, "жирный текст\n", "повторное нажатие снимает"},
    {"жирный **текст**\n", 0, 0, 12, "**жирный текст**\n",
     "наполовину жирное выделение становится жирным целиком"},
    {"обычный текст\n", 0, 3, 3, "<операция отказалась>", "без выделения документ не трогаем"},
    {"`код` и текст\n", 0, 0, 3, "<операция отказалась>",
     "внутри встроенного кода разметки не бывает"},
    {"```\nкод\n```\n", 0, 0, 3, "<операция отказалась>", "в блоке кода тоже"},
    {"| a |\n|---|\n| 1 |\n", 0, 0, 3, "<операция отказалась>",
     "и в дословном куске"},
};

const StyleCase kItalicCases[] = {
    {"обычный текст\n", 0, 0, 7, "_обычный_ текст\n", "курсивом становится выделенное"},
    {"**жирный** текст\n", 0, 0, 6, "**_жирный_** текст\n",
     "курсив ложится поверх жирного"},
};

const StyleCase kCodeCases[] = {
    {"обычный текст\n", 0, 0, 7, "`обычный` текст\n", "код в строке на выделении"},
    {"`код` и текст\n", 0, 0, 3, "код и текст\n", "повторное нажатие снимает"},
};

const StyleCase kStrikeCases[] = {
    {"обычный текст\n", 0, 0, 7, "~~обычный~~ текст\n", "зачёркнутым становится выделенное"},
    {"~~зачёркнутый~~ текст\n", 0, 0, 11, "зачёркнутый текст\n", "и снимается обратно"},
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
    for (const KeyCase& c : kBulletCases) checkKey(zametti::makeBullet, c);
    for (const KeyCase& c : kOrderedCases) checkKey(zametti::makeOrdered, c);
    for (const KeyCase& c : kTaskCases) checkKey(zametti::makeTask, c);
    for (const KeyCase& c : kParagraphCases) checkKey(zametti::makeParagraph, c);
    for (const RangeCase& c : kConvertRanges) checkRange(zametti::makeBullet, c);
    for (const RangeCase& c : kOrderedRanges) checkRange(zametti::makeOrdered, c);
    for (const RuleCase& c : kRuleCases) checkRule(c);
    for (const StyleCase& c : kBoldCases) checkStyle(zametti::toggleBold, c);
    for (const StyleCase& c : kItalicCases) checkStyle(zametti::toggleItalic, c);
    for (const StyleCase& c : kStrikeCases) checkStyle(zametti::toggleStrike, c);
    for (const StyleCase& c : kCodeCases) checkStyle(zametti::toggleCode, c);
    for (const CodeSpanCase& c : kCodeSpanCases) checkCodeSpan(c);
    for (const CodeBlockCase& c : kCodeBlockCases) checkCodeBlock(c);
    for (const PartialCodeCase& c : kPartialCodeCases) checkPartialCodeBlock(c);
    for (const char* source : {"абзац\n", "```\nраз\nдва\nтри\n```\n",
                               "абзац\n\n```\nкод\nещё\n```\n\n- пункт\n"})
        checkIrIndex(source);
    checkBulletShapes();
    for (const MoveCase& c : kMoveCases) checkMove(c);
    checkCursorAfterSplit();
    for (const char* source : kOrdinalCases)
        checkOrdinalAgreement(source, std::string("номера: ") + source);

    std::printf("проверок %d, провалов %d\n", zt::g_checks, zt::g_failures);
    return zt::g_failures == 0 ? 0 : 1;
}
