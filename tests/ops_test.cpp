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

}  // namespace

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);

    checkLevelNormalisation();
    checkGeometry();
    for (const char* source : kOrdinalCases)
        checkOrdinalAgreement(source, std::string("номера: ") + source);

    std::printf("проверок %d, провалов %d\n", zt::g_checks, zt::g_failures);
    return zt::g_failures == 0 ? 0 : 1;
}
