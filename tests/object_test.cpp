// Слой объекта: края и таблица правил.
//
// Матрица здесь ПОЛНАЯ, и это возможно ровно потому, что правило вынесено в
// чистую функцию: восемь признаков места каретки, пять клавиш — перебираются
// все сочетания целиком. Пока правила жили ветками внутри keyPressEvent,
// набор до них не дотягивался вовсе, и беды находил владелец (этап 9: три
// беды с краями картинки оказались одной, а матрица нашла ещё три).
//
// Проверяем два разных предмета:
//   * ОПОЗНАНИЕ: какому объекту принадлежит блок документа и где его края;
//   * ПРАВИЛО: что означает нажатие рядом с объектом.

#include "block_object.h"
#include "doc_model.h"
#include "document_builder.h"
#include "parser.h"
#include "test_util.h"
#include "testdata.h"

#include <QGuiApplication>
#include <QTextBlock>
#include <QTextDocument>

#include <string>
#include <vector>

namespace {

using zametti::ObjectAction;
using zametti::ObjectContext;
using zametti::ObjectKind;

std::string n(int v) { return std::to_string(v); }

std::string nameOf(ObjectKind kind) {
    switch (kind) {
        case ObjectKind::None: return "ничего";
        case ObjectKind::Image: return "картинка";
        case ObjectKind::Table: return "таблица";
        case ObjectKind::Formula: return "формула";
    }
    return "?";
}

std::string nameOf(ObjectAction action) {
    switch (action) {
        case ObjectAction::None: return "ничего";
        case ObjectAction::Edit: return "править";
        case ObjectAction::LineAfter: return "строка после";
        case ObjectAction::Remove: return "убрать";
        case ObjectAction::Select: return "выбрать";
        case ObjectAction::StepOver: return "перешагнуть";
    }
    return "?";
}

// --- опознание --------------------------------------------------------------

QTextDocument* build(const char* markdown) {
    auto* doc = new QTextDocument;
    zametti::buildDocument(zametti::parse(markdown), *doc, 1.0);
    return doc;
}

void checkRecognition() {
    // Таблица: дословный кусок, разложенный построчно. Объект обязан накрыть
    // ВСЕ её строки, включая разделитель, — иначе спрятать её целиком нечем.
    QTextDocument* doc = build(
        "текст до\n"
        "\n"
        "| имя | цена |\n"
        "|---|---:|\n"
        "| болт | 10 |\n"
        "\n"
        "текст после\n");

    std::vector<zametti::BlockObject> found;
    for (QTextBlock b = doc->begin(); b.isValid(); b = b.next()) {
        const zametti::BlockObject object = zametti::objectOf(b);
        if (!object.valid()) continue;
        if (!found.empty() && found.back().first == object.first) continue;
        found.push_back(object);
    }
    ZT_EQ("объект найден один", n(1), n(int(found.size())));
    if (!found.empty()) {
        ZT_EQ("и это таблица", nameOf(ObjectKind::Table), nameOf(found[0].kind));
        ZT_EQ("строк у неё три", n(3), n(found[0].lines()));
    }

    // Каждая строка таблицы принадлежит ей же — и первая, и разделитель, и
    // последняя. На этом ломался бы показ: спрятать надо все три.
    int inside = 0;
    for (QTextBlock b = doc->begin(); b.isValid(); b = b.next())
        if (zametti::objectOf(b).kind == ObjectKind::Table) ++inside;
    ZT_EQ("все три строки принадлежат таблице", n(3), n(inside));

    // Обычный текст объектом не является.
    ZT_EQ("абзац — не объект", nameOf(ObjectKind::None),
          nameOf(zametti::objectOf(doc->firstBlock()).kind));
    delete doc;

    // Дословный кусок, который таблицей не выглядит, объектом не становится.
    QTextDocument* raw = build("текст\n\n<div>\nчужое\n</div>\n\nещё\n");
    int objects = 0;
    for (QTextBlock b = raw->begin(); b.isValid(); b = b.next())
        if (zametti::objectOf(b).valid()) ++objects;
    ZT_EQ("чужой дословный кусок объектом не стал", n(0), n(objects));
    delete raw;

    // Картинка — один блок, и это её края.
    QTextDocument* image = build("текст\n\n![снимок](photo.jxl)\n\nещё\n");
    zametti::BlockObject photo;
    for (QTextBlock b = image->begin(); b.isValid(); b = b.next())
        if (zametti::objectOf(b).kind == ObjectKind::Image) photo = zametti::objectOf(b);
    ZT_EQ("картинка опознана", nameOf(ObjectKind::Image), nameOf(photo.kind));
    ZT_EQ("и занимает одну строку", n(1), n(photo.lines()));
    delete image;
}

// --- правила: полный перебор ------------------------------------------------
//
// Пять клавиш на все сочетания признаков. Проверяем не «ответ такой-то» для
// каждой из сотен клеток — это была бы копия реализации, — а СВОЙСТВА, которые
// обязаны держаться во всей матрице.
void checkRulesMatrix() {
    struct Key {
        const char* name;
        int key;
        Qt::KeyboardModifiers mods;
    };
    const Key keys[] = {
        {"Enter", Qt::Key_Return, Qt::NoModifier},
        {"Ctrl+Enter", Qt::Key_Return, Qt::ControlModifier},
        {"Backspace", Qt::Key_Backspace, Qt::NoModifier},
        {"Delete", Qt::Key_Delete, Qt::NoModifier},
        {"буква", Qt::Key_A, Qt::NoModifier},
    };

    int cells = 0;
    int removes = 0;
    int selects = 0;
    for (int mask = 0; mask < 512; ++mask) {
        ObjectContext where;
        where.onObject = (mask & 1) != 0;
        where.hasSelection = (mask & 2) != 0;
        where.atBlockStart = (mask & 4) != 0;
        where.atBlockEnd = (mask & 8) != 0;
        where.objectAbove = (mask & 16) != 0;
        where.objectBelow = (mask & 32) != 0;
        where.objectAboveGap = (mask & 64) != 0;
        where.objectBelowGap = (mask & 128) != 0;
        where.onGap = (mask & 256) != 0;

        for (const Key& k : keys) {
            const ObjectAction action = zametti::actionFor(k.key, k.mods, where);
            ++cells;

            // Свойство 1: при выделении слой молчит всегда. Человек правит
            // выделенный кусок текста, и объект внутри — часть этого куска.
            if (where.hasSelection)
                ZT_EQ(std::string("выделение: ") + k.name + " не трогает объект",
                      nameOf(ObjectAction::None), nameOf(action));

            // Свойство 2: печатающая клавиша слоя не касается никогда.
            if (k.key == Qt::Key_A)
                ZT_EQ(std::string("буква не трогает объект"),
                      nameOf(ObjectAction::None), nameOf(action));

            // Свойство 3: «править» и «строка после» бывают ТОЛЬКО на самом
            // объекте. Иначе Enter в обычном тексте вдруг начал бы править
            // соседнюю таблицу.
            if (action == ObjectAction::Edit || action == ObjectAction::LineAfter)
                ZT_TRUE(std::string(k.name) + ": править можно только на объекте",
                        where.onObject);

            // Свойство 4: убрать объект может только клавиша удаления.
            if (action == ObjectAction::Remove)
                ZT_TRUE(std::string(k.name) + ": убирают только Backspace и Delete",
                        k.key == Qt::Key_Backspace || k.key == Qt::Key_Delete);

            if (action == ObjectAction::Remove) ++removes;
            if (action == ObjectAction::Select) ++selects;
        }
    }
    ZT_EQ("клеток матрицы", n(2560), n(cells));
    ZT_TRUE("удаление в матрице встречается", removes > 0);
    ZT_TRUE("шаг на объект в матрице встречается", selects > 0);
}

// --- правила: названные случаи ----------------------------------------------
//
// Матрица говорит, что правила не противоречат себе; эти проверки говорят, что
// правила именно такие, о каких договорились с владельцем.
void checkNamedRules() {
    ObjectContext on;
    on.onObject = true;
    ZT_EQ("Enter на объекте — править", nameOf(ObjectAction::Edit),
          nameOf(zametti::actionFor(Qt::Key_Return, Qt::NoModifier, on)));
    ZT_EQ("Ctrl+Enter на объекте — строка после", nameOf(ObjectAction::LineAfter),
          nameOf(zametti::actionFor(Qt::Key_Return, Qt::ControlModifier, on)));
    ZT_EQ("Backspace на объекте — убрать", nameOf(ObjectAction::Remove),
          nameOf(zametti::actionFor(Qt::Key_Backspace, Qt::NoModifier, on)));
    ZT_EQ("Delete на объекте — убрать", nameOf(ObjectAction::Remove),
          nameOf(zametti::actionFor(Qt::Key_Delete, Qt::NoModifier, on)));

    // Края: удаление «в сторону объекта» убирает его целиком.
    ObjectContext below;
    below.atBlockStart = true;
    below.objectAbove = true;
    ZT_EQ("Backspace в начале строки под объектом — убрать его",
          nameOf(ObjectAction::Remove),
          nameOf(zametti::actionFor(Qt::Key_Backspace, Qt::NoModifier, below)));
    // А не в начале строки — обычная правка текста.
    ObjectContext middle = below;
    middle.atBlockStart = false;
    ZT_EQ("Backspace в середине строки — обычная правка", nameOf(ObjectAction::None),
          nameOf(zametti::actionFor(Qt::Key_Backspace, Qt::NoModifier, middle)));

    ObjectContext above;
    above.atBlockEnd = true;
    above.objectBelow = true;
    ZT_EQ("Delete в конце строки над объектом — убрать его", nameOf(ObjectAction::Remove),
          nameOf(zametti::actionFor(Qt::Key_Delete, Qt::NoModifier, above)));

    // Пустая строка между объектом и текстом неудаляема: отказ и шаг.
    ObjectContext gapAbove;
    gapAbove.atBlockStart = true;
    gapAbove.objectAboveGap = true;
    ZT_EQ("через пустую строку — шаг на объект", nameOf(ObjectAction::Select),
          nameOf(zametti::actionFor(Qt::Key_Backspace, Qt::NoModifier, gapAbove)));

    ObjectContext onGap;
    onGap.onGap = true;
    onGap.objectAbove = true;
    ZT_EQ("с самой пустой строки Backspace — шаг на объект", nameOf(ObjectAction::Select),
          nameOf(zametti::actionFor(Qt::Key_Backspace, Qt::NoModifier, onGap)));
    onGap.objectAbove = false;
    onGap.objectBelow = true;
    ZT_EQ("и Delete — тоже шаг", nameOf(ObjectAction::Select),
          nameOf(zametti::actionFor(Qt::Key_Delete, Qt::NoModifier, onGap)));
}

}  // namespace

static int ztRunSuite(int argc, char** argv) {
    (void)argc;
    (void)argv;
    checkRecognition();
    checkRulesMatrix();
    checkNamedRules();
    return zt::report("объекты");
}

// Набор целиком одним TEST: тело не тронуто, argv ему собран здесь.
// Дробить на отдельные проверки — отдельная работа, по одному набору.
TEST(Object, All) {
    std::vector<QByteArray> ztArgs{QByteArrayLiteral("object_test")};
    std::vector<char*> ztArgv;
    for (QByteArray& a : ztArgs) ztArgv.push_back(a.data());
    EXPECT_EQ(0, ztRunSuite(int(ztArgv.size()), ztArgv.data()));
}
