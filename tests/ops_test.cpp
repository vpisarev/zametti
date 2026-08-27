// Операции редактирования: IR до → операция → IR после.
//
// Документ собирается из IR, над ним работает операция, результат читается
// обратно и сравнивается по сериализованному виду — так расхождение читается
// глазами, а не по номерам полей.
//
// СМОТРИМ ЖИВОЙ ДОКУМЕНТ (liveMarkdown), А НЕ ФАЙЛ (toMarkdown). Предмет этого
// набора — что операция сделала с документом ПРЯМО СЕЙЧАС, вместе со всем, чего
// markdown не хранит: пустой абзац под кареткой, пустой вложенный пункт,
// хвостовой пробел. Файловый канон всё это законно снимает — и тогда «Enter в
// пустом пункте снял список, каретка в пустом абзаце» стало бы неотличимо от
// «пункт просто исчез». Записывается ли то, что вышло, спрашивает другой набор
// (FuzzOps), и спрашивает он ровно про запись.

#include "doc_model.h"
#include "pieces.h"
#include "document_builder.h"
#include "editor_ops.h"
#include "marker.h"
#include "settings_hook.h"
#include "test_util.h"
#include "testdata.h"

#include <QGuiApplication>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>

#include <string>
#include <vector>

namespace {

using zametti::Piece;

using zametti::Kind;
using zametti::Marker;

// Правка — ГЛАГОЛОМ ЗАМЕТКИ, ровно тем же, каким её зовёт редактор. Набор
// стережёт ту дверь, через которую ходит приложение, а не соседнюю: свободные
// функции разметки живут внутри ядра и наружу не выходят.
using NoteOp = std::function<bool(zametti::ZDocument&, QTextCursor&)>;

// Набросок блока: короткий литерал для наборов. Настоящий Piece держит ещё
// куски разметки и признак завершающего перевода строки, а здесь нужен только
// скелет.
struct Sketch {
    Kind kind = Kind::Paragraph;
    Marker marker = Marker::Bullet;
    int level = -1;
    const char* text = "";
};

std::vector<zametti::Piece> docOf(const std::vector<Sketch>& sketches) {
    std::vector<zametti::Piece> ir;
    for (const Sketch& p : sketches) {
        zametti::Piece b;
        b.kind = p.kind;
        b.text = p.text;
        b.marker = p.marker;
        b.level = p.level;
        ir.push_back(std::move(b));
    }
    return ir;
}

std::vector<zametti::Piece> docOf(std::initializer_list<Sketch> sketches) {
    return docOf(std::vector<Sketch>(sketches));
}

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

Sketch listItem(Marker marker, int level, const char* text) {
    return {Kind::ListItem, marker, level, text};
}

Sketch paragraph(const char* text) { return {Kind::Paragraph, Marker::Bullet, -1, text}; }

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
void checkSync(std::initializer_list<Sketch> before, const char* expectedLevels,
               const char* what) {
    QTextDocument doc;
    zametti::buildDocument(docOf(before), doc);
    zametti::syncLists(doc, {0, doc.blockCount() - 1});

    checkEqual(expectedLevels, levelsToString(levelsOf(doc)), what);

    QString problem;
    check(zametti::listInvariantHolds(doc, &problem),
          std::string(what) + ": инвариант нарушен — " + problem.toStdString());

    // Текст не должен пострадать: операция про уровни.
    const std::vector<zametti::Piece> after = blocksOf(doc);
    check(after.size() == before.size(), std::string(what) + ": число блоков изменилось");
    for (size_t i = 0; i < after.size() && i < before.size(); ++i)
        check(after[i].text == (before.begin() + i)->text,
              std::string(what) + ": текст блока изменился");
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
    std::vector<Sketch> blocks;
    for (int i = 0; i < 12; ++i) blocks.push_back(listItem(Marker::Ordered, 0, "пункт"));
    blocks.push_back(listItem(Marker::Ordered, 1, "вложенный"));
    const std::vector<zametti::Piece> doc = docOf(blocks);

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
    zametti::buildDocument(docOf({listItem(Marker::Bullet, 0, "буллет"),
                                  listItem(Marker::Task, 0, "задача"),
                                  listItem(Marker::Bullet, 0, "снова буллет")}),
                           mixed);
    check(marginOf(mixed, 0) < marginOf(mixed, 1),
          "кружок не равняется по ширине чекбокса");
    check(marginOf(mixed, 0) == marginOf(mixed, 2),
          "оба кружка стоят одинаково, задача между ними им не мешает");

    // И тот же кружок сам по себе стоит там же, где рядом с задачами.
    QTextDocument alone;
    zametti::buildDocument(docOf({listItem(Marker::Bullet, 0, "буллет")}), alone);
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
    zametti::buildDocument(pieces(source), doc);

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

// ДОСЛОВНЫЙ КУСОК — ОДИН QTextBlock (сессия 5 refactor2): его строки — U+2028
// внутри блока, как у кода. Блок заметки == блок документа, номер IR — номер
// блока; продолжений больше нет, и рвать нечего. Проверяем строение и круг.
void checkLiteralOneBlock() {
    QTextDocument doc;
    const std::vector<zametti::Piece> ir =
        pieces("абзац\n\n| a | b |\n|---|---|\n| 1 | 2 |\n\n<div>\nдва\n</div>\n");
    zametti::buildDocument(ir, doc);
    checkEqual(std::to_string(ir.size()), std::to_string(doc.blockCount()),
               "блоков документа столько же, сколько блоков заметки");
    for (int i = 0; i < doc.blockCount(); ++i) {
        const QTextBlock block = doc.findBlockByNumber(i);
        checkEqual(ir[size_t(i)].raw ? "raw" : "kind",
                   zametti::isRawBlock(block) ? "raw" : "kind",
                   "блок " + std::to_string(i) + " того же рода");
    }
    // Строки дословного куска — внутри одного блока, и текст файла тот же.
    const QTextBlock table = doc.findBlockByNumber(2);
    check(zametti::isRawBlock(table) && zametti::sourceTextOf(table).count(u'\n') == 2,
          "таблица лежит одним блоком с двумя переводами внутри");
    checkEqual("абзац\n\n| a | b |\n|---|---|\n| 1 | 2 |\n\n<div>\nдва\n</div>\n",
               markdownOf(blocksOf(doc)), "круг через документ байт в байт");
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

void checkKey(const NoteOp& op, const KeyCase& c) {
    zametti::ZDocument note = bodyOf(c.before);

    QTextCursor cursor = note.caretAtBlock(c.block);
    check(cursor.blockNumber() == c.block, std::string(c.what) + ": нет такого блока");
    cursor.setPosition(cursor.position() + c.offset);

    const bool handled = op(note, cursor);
    const std::string actual = handled ? note.liveMarkdown().toStdString() : std::string("<операция отказалась>");
    checkEqual(c.after, actual, c.what);

    if (!handled) return;
    const QString problem = note.structureProblem();
    check(problem.isEmpty(), std::string(c.what) + ": строение — " + problem.toStdString());
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
    // Пустой вложенный пункт выходит из списка сразу, а не поднимается на
    // уровень: из вложенного списка иначе пришлось бы выходить тремя нажатиями.
    {"- верх\n  - вложенный\n  - \n", 2, 0, "- верх\n  - вложенный\n\n\n",
     "пустой вложенный тоже снимает список"},

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
    // Блок кода — один QTextBlock: пустая вторая строка стоит в нём за
    // разделителем, на смещении 4.
    {"```\nкод\n\n```\n", 0, 4, "```\nкод\n\n\n```\n",
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
    // С чужим списком тексты в одну строку не сливаются, но разжалованная
    // строка пристаёт к пункту выше мягким переносом: строки на экране как
    // стояли, так и стоят, и документ от Backspace не растёт.
    {"- верх\n  - вложенный\n", 1, 0, "- верх\n  вложенный\n",
     "вложенный теряет маркер и пристаёт строкой к родителю"},
    {"- буллет\n- [ ] задача\n", 1, 0, "- буллет\n  задача\n",
     "задача теряет маркер и пристаёт строкой к буллету"},
    {"1. номер\n- буллет\n", 1, 0, "1. номер\n   буллет\n",
     "буллет теряет маркер и пристаёт строкой к номеру"},
    {"- пустой\n- \n", 1, 0, "- пустой\n", "пустой пункт просто исчезает"},
    // Дальше — не жесты списка, а обычное удаление знака: набор спрашивает
    // BACKSPACE ЦЕЛИКОМ (глагол заметки), а не одну его составляющую, как
    // прежде. Внутри текста он съедает букву, а в самом начале заметки слева
    // ничего нет и отказывается.
    {"- пункт\n", 0, 3, "- пукт\n", "внутри текста удаляется знак"},
    {"абзац\n", 0, 0, "<операция отказалась>", "в начале заметки удалять нечего"},
    {"```\nкод\n```\n", 0, 0, "<операция отказалась>", "в коде в начале заметки тоже"},
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
    // ТОЛЬКО СТРОКА КАРЕТКИ, НЕ ПОДДЕРЕВО (решение владельца, сессия 9): дети
    // остаются на своём уровне и становятся братьями сдвинутого пункта — ровно
    // так прочёл бы файл md4c, и так же ведёт себя режим исходника. Прежде
    // поддерево ехало вместе с пунктом даже без выделения.
    {"- раз\n- два\n  - вложенный\n", 1, 0, "- раз\n  - два\n  - вложенный\n",
     "дети сдвинутого пункта становятся его братьями"},
    // Абзац-продолжение пункта за его детьми остаётся на месте — и достаётся
    // пункту того же уровня выше (так читает файл md4c; особого случая нет).
    // Через пустую строку: без неё «  хвост» — ленивое продолжение «внука».
    {"- раз\n- два\n  - внук\n\n  хвост\n", 1, 0, "- раз\n  - два\n  - внук\n\n  хвост\n",
     "продолжение за детьми остаётся на месте"},
    {"- раз\n- два\n  - внук\n  хвост\n", 1, 0, "- раз\n  - два\n  - внук\n    хвост\n",
     "ленивое продолжение внука едет с внуком — это его текст"},

    // БЛОК КОДА ДВИГАЕТСЯ ЦЕЛИКОМ — в начале блока, где Tab не занят отступом
    // текста. Внутри пункта он становится содержимым пункта, и список за ним
    // продолжает нумерацию (просил владелец).
    {"1. раз\n2. два\n\n```\nкод\nещё\n```\n\n3. три\n", 3, 0,
     "1. раз\n2. два\n\n   ```\n   код\n   ещё\n   ```\n\n3. три\n",
     "блок кода уходит внутрь пункта весь, обеими строками"},
    // Дальше первой колонки Tab остаётся отступом кода: строка сдвигается, блок
    // стоит на месте.
    {"1. раз\n\n```\nкод\n```\n", 2, 3,
     "1. раз\n\n```\nкод \n```\n",
     "в конце строки Tab по-прежнему отступ внутри кода (до ближайшего стопа)"},
    // Вторая строка блока — не начало блока: там Tab тоже отступ текста.
    // Вторая строка блока — за разделителем внутри того же QTextBlock (2), на
    // смещении 4.
    {"1. раз\n\n```\nкод\nещё\n```\n", 2, 4,
     "1. раз\n\n```\nкод\n    ещё\n```\n",
     "начало ВТОРОЙ строки кода двигает строку, а не блок"},
    {"```\nкод\n```\n", 0, 0, "```\n    код\n```\n",
     "без пункта выше двигать некуда — остаётся отступ текста"},
    // СЛУЧАЙ ВЛАДЕЛЬЦА, вживую: «About the Scopes» в Ficus Tutorial. Список
    // разорван блоком кода пополам, и оттого нумерация за кодом начинается
    // заново. Tab в начале кода вбирает его во второй пункт — и два списка
    // сходятся в один, продолжая счёт.
    //
    // Счёт при этом идёт с единицы, а не с шестёрки: НОМЕРА НЕ ХРАНЯТСЯ, их
    // считает ordinalOf по прогону. Исходные «6.» и «7.» становятся первым и
    // вторым пунктом уже при чтении файла, до всякой правки.
    {"6. Names never conflict.\n7. Types never conflict:\n\n```\ntype M=string\n```\n\n"
     "1. Functions may have the same name.\n2. Values may have the same name.\n",
     3, 0,
     "1. Names never conflict.\n2. Types never conflict:\n\n   ```\n   type M=string\n   ```\n\n"
     "3. Functions may have the same name.\n4. Values may have the same name.\n",
     "разорванный кодом список сходится в один"},
};

const KeyCase kOutdentCases[] = {
    {"- раз\n  - два\n", 1, 0, "- раз\n- два\n", "пункт выходит на уровень выше"},
    {"- раз\n- два\n", 1, 0, "<операция отказалась>", "с верхнего уровня выходить некуда"},
    // Выступ двигает тоже только строку каретки, но дети при этом прижимаются
    // на уровень: прыжка через уровень в файле не бывает (syncLists). Итог тот
    // же, что был у «поддерево выходит вместе с пунктом».
    {"- раз\n  - два\n    - три\n", 1, 0, "- раз\n- два\n  - три\n",
     "дети выступившего пункта прижимаются к нему"},
    {"абзац\n", 0, 0, "<операция отказалась>", "вне списка Shift+Tab не при чём"},

    // И обратно: блок кода выходит из пункта целиком.
    {"1. раз\n2. два\n\n   ```\n   код\n   ещё\n   ```\n\n3. три\n", 3, 0,
     "1. раз\n2. два\n\n```\nкод\nещё\n```\n\n1. три\n",
     "блок кода выходит из пункта весь"},
};

const KeyCase kToggleCases[] = {
    {"- [ ] дело\n", 0, 0, "- [x] дело\n", "невыполненная становится выполненной"},
    {"- [x] дело\n", 0, 0, "- [ ] дело\n", "и обратно"},
    {"- буллет\n", 0, 0, "<операция отказалась>", "буллет не задача"},
    // Переключается ТОЛЬКО строка каретки: вложенные задачи не трогаются
    // (решение владельца, сессия 9; прежде шло всё поддерево).
    {"- [ ] дело\n  - [ ] часть\n", 0, 0, "- [x] дело\n  - [ ] часть\n",
     "вложенная задача при переключении родителя не трогается"},
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

void checkRange(const NoteOp& op, const RangeCase& c) {
    zametti::ZDocument note = bodyOf(c.before);

    QTextCursor cursor = note.caretAtBlock(c.firstBlock);
    QTextCursor tail = note.caretAtBlock(c.lastBlock);
    tail.movePosition(QTextCursor::EndOfBlock);
    cursor.setPosition(tail.position(), QTextCursor::KeepAnchor);

    const bool handled = op(note, cursor);
    const std::string actual = handled ? note.liveMarkdown().toStdString() : std::string("<операция отказалась>");
    checkEqual(c.after, actual, c.what);

    if (!handled) return;
    const QString problem = note.structureProblem();
    check(problem.isEmpty(), std::string(c.what) + ": строение — " + problem.toStdString());
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
    {"- [ ] дело\n  - [ ] часть\n", 0, 1, "- [x] дело\n  - [x] часть\n",
     "выделение с вложенной — обе"},
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
    zametti::ZDocument note = bodyOf(c.before);

    QTextCursor cursor = note.caretAtBlock(note.blockCount() - 1);
    cursor.movePosition(QTextCursor::End);
    note.insertText(cursor, QString::fromUtf8(c.typed));

    cursor.movePosition(QTextCursor::End);
    const bool handled = note.applyCodeSpanRule(cursor);
    const std::string actual = handled ? note.liveMarkdown().toStdString()
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
    zametti::ZDocument note = bodyOf(c.source);

    QTextCursor cursor = note.caretAtBlock(c.firstBlock);
    QTextCursor tail = note.caretAtBlock(c.lastBlock);
    tail.movePosition(QTextCursor::EndOfBlock);
    cursor.setPosition(tail.position(), QTextCursor::KeepAnchor);

    const bool done = note.toggleCodeBlock(cursor);
    checkEqual(c.after, done ? note.liveMarkdown().toStdString() : std::string("<операция отказалась>"), c.what);
}

// Выделение частью блока: абзац с мягкими переносами — один блок, а строк в
// нём много. В блок кода должно уйти только выделенное.
struct PartialCodeCase {
    const char* source;
    int from;
    int to;
    const char* after;
    const char* what;
    int block = 0;      // от какого блока считаются смещения
};

void checkPartialCodeBlock(const PartialCodeCase& c) {
    zametti::ZDocument note = bodyOf(c.source);

    QTextCursor cursor = note.caretAtBlock(c.block);
    const int base = cursor.position();
    cursor.setPosition(base + c.from);
    cursor.setPosition(base + c.to, QTextCursor::KeepAnchor);

    const bool done = note.toggleCodeBlock(cursor);
    checkEqual(c.after, done ? note.liveMarkdown().toStdString() : std::string("<операция отказалась>"), c.what);
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

    // ПУНКТ С МЯГКИМИ ПЕРЕНОСАМИ — тоже один блок, и строк в нём много. Средняя
    // строка уходит в код, а пункт при этом обязан УЦЕЛЕТЬ: и голова, и хвост
    // остаются тем же пунктом, только хвост — уже его продолжением, без своего
    // маркера. Прежде хвост становился НОВЫМ пунктом, и список разъезжался.
    // «два\nвторая\nтретья» — по семи знакам на строку с разделителем.
    {"1. раз\n2. два\n   вторая\n   третья\n3. три\n", 4, 10,
     "1. раз\n2. два\n\n   ```\n   вторая\n   ```\n   третья\n3. три\n",
     "средняя строка пункта уходит в код, пункт цел", 1},
};

const CodeBlockCase kCodeBlockCases[] = {
    {"абзац\n", 0, 0, "```\nабзац\n```\n", "абзац становится блоком кода"},
    {"раз\n\nдва\n", 0, 2, "```\nраз\n\nдва\n```\n",
     "два абзаца сливаются в один блок"},
    {"```\nкод\n```\n", 0, 0, "код\n", "блок кода возвращается в текст"},
    {"```\nраз\nдва\n```\n", 0, 1, "раз\nдва\n",
     "многострочный код становится одним абзацем"},
    // Отступы внутри кода при обратном ходе становятся НЕРАЗРЫВНЫМИ прямо в
    // операции. Прежде они оставались обычными, а неразрывными их делала запись
    // в файл; теперь это делает сама операция — вместе с сериями пробелов в
    // середине строки, которые запись не трогает вовсе, а markdown схлопывает.
    // Без этого столбик "int a     = 5" превращался бы в "int a = 5".
    {"```\nif x:\n    y\n```\n", 0, 1,
     "if x:\n\xC2\xA0\xC2\xA0\xC2\xA0\xC2\xA0y\n",
     "отступ кода переживает возврат в текст"},
    {"- пункт\n", 0, 0, "```\nпункт\n```\n", "пункт списка тоже можно"},

    // КОД ВНУТРИ ПУНКТА. Уровень — такая же принадлежность блока, как род, и
    // операция обязана его сохранить: иначе блок кода выпадает из списка, а
    // список за ним начинает нумерацию заново (нашёл владелец).
    {"1. раз\n2. два\n\n   второй абзац\n\n3. три\n", 3, 3,
     "1. раз\n2. два\n\n   ```\n   второй абзац\n   ```\n\n3. три\n",
     "второй абзац пункта становится кодом ВНУТРИ пункта"},
    {"1. раз\n2. два\n\n   ```\n   второй абзац\n   ```\n\n3. три\n", 3, 3,
     "1. раз\n2. два\n\n   второй абзац\n\n3. три\n",
     "и возвращается обратно тоже внутрь пункта"},
    // Сам пункт становится кодом — на своём уровне: список продолжается, а
    // текст пункта уезжает в код целиком (маркера у кода не бывает).
    // Пустая строка нужна ПЕРЕД кодом внутри пункта и не нужна за ним: строка
    // "   ```" сразу за строкой пункта читается его продолжением, а пункт сразу
    // за закрывающим забором читается пунктом (замерено на md4c).
    {"1. раз\n2. два\n3. три\n", 1, 1,
     "1. раз\n\n   ```\n   два\n   ```\n2. три\n",
     "пункт, ставший кодом, остаётся внутри списка"},
};

// Фигура буллета зависит от уровня вложенности: так вложенность видна сразу.
// Последняя фигура достаётся всем уровням глубже — иначе на пятом уровне буллет
// пропал бы вовсе.
void checkBulletShapes() {
    const std::vector<zametti::BulletShape> saved = zametti::settings().style().bulletShapes();

    zametti::mutableSettingsForTests().style().setBulletShapes({zametti::BulletShape::Disc,
                                                 zametti::BulletShape::Circle,
                                                 zametti::BulletShape::Square});
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
    zametti::mutableSettingsForTests().style().setBulletShapes({});
    check(zametti::bulletShapeFor(0) == zametti::BulletShape::Disc,
          "без настроенных фигур остаётся сплошной кружок");

    zametti::mutableSettingsForTests().style().setBulletShapes(saved);
}

// Автозамена: набранное в начале блока, положение курсора после пробела.
struct RuleCase {
    const char* typed;     // что оказалось в блоке к моменту проверки
    const char* before;    // документ до
    const char* after;
    const char* what;
};

void checkRule(const RuleCase& c) {
    zametti::ZDocument note = bodyOf(c.before);

    // Набираем в начало первого блока — ровно так, как это делает человек.
    QTextCursor cursor = note.caretAtBlock(0);
    note.insertText(cursor, QString::fromUtf8(c.typed));

    const bool handled = note.applyInputRule(cursor);
    const std::string actual = handled ? note.liveMarkdown().toStdString()
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

void checkStyle(zametti::ZDocument::Style style, const StyleCase& c) {
    zametti::ZDocument note = bodyOf(c.before);

    QTextCursor cursor = note.caretAtBlock(c.block);
    const int base = cursor.position();
    cursor.setPosition(base + c.from);
    if (c.to > c.from) cursor.setPosition(base + c.to, QTextCursor::KeepAnchor);

    const bool handled = note.toggleStyle(cursor, style);
    const std::string actual = handled ? note.liveMarkdown().toStdString() : std::string("<операция отказалась>");
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
    zametti::ZDocument note = bodyOf(c.before);
    const std::string wasText = note.blockAt(c.block).text.toStdString();

    QTextCursor cursor = note.caretAtBlock(c.block);
    const bool moved = note.moveListItem(cursor, c.direction);
    const std::string actual = moved ? note.liveMarkdown().toStdString() : std::string("<операция отказалась>");
    checkEqual(c.after, actual, c.what);
    if (!moved) return;

    // Каретка обязана уехать вместе с пунктом: иначе она осталась бы в чужом.
    checkEqual(wasText, note.blockAt(cursor.blockNumber()).text.toStdString(),
               std::string(c.what) + ": каретка осталась в том же пункте");
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
    zametti::ZDocument note = bodyOf("- пунктхвост\n");
    QTextCursor cursor = note.caretAtBlock(0);
    cursor.setPosition(cursor.position() + 5);
    note.breakBlock(cursor, zametti::ZDocument::BreakKind::Plain);
    check(cursor.blockNumber() == 1 && cursor.positionInBlock() == 0,
          "курсор после разреза стоит в начале нового блока");
}

}  // namespace

static int ztRunSuite(int argc, char** argv) {
    (void)argc;
    (void)argv;

    checkLevelNormalisation();
    checkGeometry();
    checkLiteralOneBlock();
    using Z = zametti::ZDocument;
    const NoteOp enter = [](Z& n, QTextCursor& at) { return n.breakBlock(at, Z::BreakKind::Plain); };
    const NoteOp indent = [](Z& n, QTextCursor& at) { return n.indent(at); };
    const NoteOp outdent = [](Z& n, QTextCursor& at) { return n.outdent(at); };
    const NoteOp toggleTask = [](Z& n, QTextCursor& at) { return n.toggleTask(at); };
    const NoteOp bullet = [](Z& n, QTextCursor& at) { return n.makeBullet(at); };
    const NoteOp ordered = [](Z& n, QTextCursor& at) { return n.makeOrdered(at); };

    for (const KeyCase& c : kEnterCases) checkKey(enter, c);
    for (const KeyCase& c : kBackspaceCases)
        checkKey([](Z& n, QTextCursor& at) { return n.deleteBack(at); }, c);
    for (const KeyCase& c : kIndentCases) checkKey(indent, c);
    for (const KeyCase& c : kOutdentCases) checkKey(outdent, c);
    for (const KeyCase& c : kToggleCases) checkKey(toggleTask, c);
    for (const RangeCase& c : kIndentRanges) checkRange(indent, c);
    for (const RangeCase& c : kToggleRanges) checkRange(toggleTask, c);
    for (const KeyCase& c : kBulletCases) checkKey(bullet, c);
    for (const KeyCase& c : kOrderedCases) checkKey(ordered, c);
    for (const KeyCase& c : kTaskCases)
        checkKey([](Z& n, QTextCursor& at) { return n.makeTask(at); }, c);
    for (const KeyCase& c : kParagraphCases)
        checkKey([](Z& n, QTextCursor& at) { return n.makeParagraph(at); }, c);
    for (const RangeCase& c : kConvertRanges) checkRange(bullet, c);
    for (const RangeCase& c : kOrderedRanges) checkRange(ordered, c);
    for (const RuleCase& c : kRuleCases) checkRule(c);
    for (const StyleCase& c : kBoldCases) checkStyle(Z::Style::Bold, c);
    for (const StyleCase& c : kItalicCases) checkStyle(Z::Style::Italic, c);
    for (const StyleCase& c : kStrikeCases) checkStyle(Z::Style::Strike, c);
    for (const StyleCase& c : kCodeCases) checkStyle(Z::Style::Code, c);
    for (const CodeSpanCase& c : kCodeSpanCases) checkCodeSpan(c);
    for (const CodeBlockCase& c : kCodeBlockCases) checkCodeBlock(c);
    for (const PartialCodeCase& c : kPartialCodeCases) checkPartialCodeBlock(c);
    checkBulletShapes();
    for (const MoveCase& c : kMoveCases) checkMove(c);
    checkCursorAfterSplit();
    for (const char* source : kOrdinalCases)
        checkOrdinalAgreement(source, std::string("номера: ") + source);

    std::printf("проверок %d, провалов %d\n", zt::g_checks, zt::g_failures);
    return zt::freshFailures();
}

// Набор целиком одним TEST: тело не тронуто, argv ему собран здесь.
// Дробить на отдельные проверки — отдельная работа, по одному набору.
TEST(Ops, All) {
    std::vector<QByteArray> ztArgs{QByteArrayLiteral("ops_test")};
    std::vector<char*> ztArgv;
    for (QByteArray& a : ztArgs) ztArgv.push_back(a.data());
    EXPECT_EQ(0, ztRunSuite(int(ztArgv.size()), ztArgv.data()));
}
