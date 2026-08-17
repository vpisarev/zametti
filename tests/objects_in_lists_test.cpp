// ОБЪЕКТЫ ВНУТРИ ПУНКТОВ СПИСКА — ЛЮБОЙ ГЛУБИНЫ (решение владельца, сессия 5).
//
// Формула, картинка, таблица и блок кода живут внутри пункта на его уровне:
// файл читается и пишется байт в байт, объект стоит с отступом пункта,
// Tab/Shift+Tab двигают его уровень, Ctrl+Enter заводит новый пункт под ним,
// Shift+Enter продолжает ТОТ ЖЕ пункт текстом, а Backspace в пустом пункте
// под объектом убирает пункт, а не объект («удаляется и пункт, и формула» —
// названный владельцем дефект). Матрица — до починок; клавиши — как у
// человека, через редактор.

#include "block_object.h"
#include "doc_model.h"
#include "editor_ops.h"
#include "editor_widget.h"
#include "formula.h"
#include "pieces.h"
#include "resources.h"
#include "test_util.h"
#include "testdata.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QTest>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>

#include <string>
#include <vector>

namespace {

QString g_dir;

std::string n(int v) { return std::to_string(v); }

QString writeNote(const QString& name, const char* text) {
    const QString path = QDir(g_dir).filePath(name + QStringLiteral(".md"));
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return {};
    f.write(text);
    f.close();
    return path;
}

class Editor : public zametti::NoteEditor {
public:
    void openText(const QString& name, const char* text) {
        const QString path = writeNote(name, text);
        resize(900, 700);
        show();
        QTest::qWait(10);
        openFile(path);
        QTest::qWait(30);
    }
    std::string markdown() const { return markdownOf(blocksOf(*document())); }

    // Блок-объект с таким исходником (формула — по исходнику, картинка — по
    // подписи, таблица — по первой строке); -1 — нет.
    int objectBlock(zametti::ObjectKind kind, const QString& mark) const {
        for (QTextBlock b = document()->firstBlock(); b.isValid(); b = b.next()) {
            const zametti::BlockObject object = zametti::objectOf(b);
            if (object.kind != kind) continue;
            if (mark.isEmpty() || zametti::sourceTextOf(b).contains(mark) ||
                zametti::blockFormulaRef(b).source.contains(mark) ||
                zametti::blockImageRef(b).alt.contains(mark))
                return b.blockNumber();
        }
        return -1;
    }
    int codeBlock() const {
        for (QTextBlock b = document()->firstBlock(); b.isValid(); b = b.next())
            if (!zametti::isRawBlock(b) && zametti::kindOf(b) == zametti::Kind::Code)
                return b.blockNumber();
        return -1;
    }
    void caretToBlock(int number, bool atEnd = false) {
        QTextCursor at(document()->findBlockByNumber(number));
        if (atEnd) at.movePosition(QTextCursor::EndOfBlock);
        setTextCursor(at);
    }
    // Что стоит в блоке: род/уровень/пусто — коротко, для сообщений.
    std::string describe(int number) const {
        const QTextBlock b = document()->findBlockByNumber(number);
        if (!b.isValid()) return "нет блока";
        std::string out = zametti::isRawBlock(b) ? "raw" : n(int(zametti::kindOf(b)));
        out += " level=" + n(zametti::levelOf(b));
        out += b.text().isEmpty() ? " пусто" : " «" + b.text().left(12).toStdString() + "»";
        return out;
    }
};

// Документ законен: инварианты и круг файла.
void checkLegal(Editor& editor, const std::string& where) {
    QString problem;
    ZT_TRUE(where + ": инвариант пустых строк (" + problem.toStdString() + ")",
            zametti::gapInvariantHolds(*editor.document(), &problem));
    ZT_TRUE(where + ": инвариант списков (" + problem.toStdString() + ")",
            zametti::listInvariantHolds(*editor.document(), &problem));
    const std::string text = editor.markdown();
    ZT_EQ(where + ": круг файла", text, markdownOf(pieces(text)));
}

// --- фикстура со всеми видами на трёх уровнях -------------------------------

const char* kAll =
    "- item\n"
    "\n"
    "  ![pic](img.png)\n"
    "\n"
    "- second\n"
    "  1. nested\n"
    "\n"
    "     $$x^2$$\n"
    "\n"
    "     | a | b |\n"
    "     |---|---|\n"
    "     | 1 | 2 |\n"
    "\n"
    "     ```py\n"
    "     x = 1\n"
    "     ```\n"
    "\n"
    "     - deep\n"
    "\n"
    "       $$y$$\n"
    "\n"
    "- last\n"
    "\n"
    "  $$z$$\n";

void checkRoundTripAndLevels() {
    Editor editor;
    editor.openText(QStringLiteral("all"), kAll);
    ZT_EQ("файл со всеми видами читается и пишется байт в байт", std::string(kAll),
          editor.markdown());
    checkLegal(editor, "фикстура");

    struct Want {
        zametti::ObjectKind kind;
        const char* mark;
        int level;
    };
    const Want wants[] = {
        {zametti::ObjectKind::Image, "pic", 0},
        {zametti::ObjectKind::Formula, "x^2", 1},
        {zametti::ObjectKind::Table, "| a | b |", 1},
        {zametti::ObjectKind::Formula, "$$y$$", 2},
        {zametti::ObjectKind::Formula, "$$z$$", 0},
    };
    qreal previousMargin = -1.0;
    for (const Want& want : wants) {
        const int number = editor.objectBlock(want.kind, QString::fromUtf8(want.mark));
        ZT_TRUE(std::string("объект «") + want.mark + "» найден", number >= 0);
        if (number < 0) continue;
        const QTextBlock block = editor.document()->findBlockByNumber(number);
        ZT_EQ(std::string("уровень объекта «") + want.mark + "»", n(want.level),
              n(zametti::levelOf(block)));
        // Полоса объекта отступает вместе с пунктом: чем глубже, тем правее.
        ZT_TRUE(std::string("объект «") + want.mark + "» отступает как пункт (поле " +
                    n(int(block.blockFormat().leftMargin())) + ")",
                block.blockFormat().leftMargin() > 0.0);
        (void)previousMargin;
    }
    const int deep = editor.objectBlock(zametti::ObjectKind::Formula, QStringLiteral("$$y$$"));
    const int shallow = editor.objectBlock(zametti::ObjectKind::Formula, QStringLiteral("$$z$$"));
    if (deep >= 0 && shallow >= 0)
        ZT_TRUE("объект глубже — правее",
                editor.document()->findBlockByNumber(deep).blockFormat().leftMargin() >
                    editor.document()->findBlockByNumber(shallow).blockFormat().leftMargin());
    // Снимок — артефакт приёмки: маркеры рядом с полосами объектов, отступы по
    // глубине — на него смотрит владелец.
    editor.grab().toImage().save(QDir(g_dir).filePath(QStringLiteral("объекты-в-пунктах.png")));
    const int code = editor.codeBlock();
    ZT_TRUE("блок кода найден", code >= 0);
    if (code >= 0)
        ZT_EQ("уровень блока кода", n(1),
              n(zametti::levelOf(editor.document()->findBlockByNumber(code))));
}

// --- Ctrl+Enter: новый пункт под объектом; Backspace убирает только его ------

void checkNewItemAfterObject() {
    Editor editor;
    editor.openText(QStringLiteral("ctrl-enter"), "- item\n\n  $$z$$\n");
    const int formula = editor.objectBlock(zametti::ObjectKind::Formula, QStringLiteral("$$z$$"));
    ZT_TRUE("формула в пункте есть", formula >= 0);
    if (formula < 0) return;
    editor.caretToBlock(formula);
    QTest::keyClick(&editor, Qt::Key_Return, Qt::ControlModifier);
    QTest::qWait(20);

    // Каретка — в новом ПУСТОМ ПУНКТЕ на том же уровне; формула цела.
    const QTextBlock here = editor.textCursor().block();
    ZT_TRUE("после Ctrl+Enter каретка в пункте списка (" + editor.describe(here.blockNumber()) + ")",
            zametti::isListBlock(here) && here.text().isEmpty());
    ZT_EQ("новый пункт на уровне объекта", n(0), n(zametti::levelOf(here)));
    ZT_TRUE("формула цела", editor.objectBlock(zametti::ObjectKind::Formula, QStringLiteral("$$z$$")) >= 0);
    checkLegal(editor, "после Ctrl+Enter");

    // Набрали — пункт с текстом; в файле — пункт после формулы.
    QTest::keyClicks(&editor, QStringLiteral("next"));
    QTest::qWait(20);
    ZT_EQ("файл после Ctrl+Enter и набора", std::string("- item\n\n  $$z$$\n\n- next\n"),
          editor.markdown());
    checkLegal(editor, "после набора в новом пункте");

    // Ctrl+Z ×2 — назад к пустому пункту и к исходному файлу.
    QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier);
    QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier);
    QTest::qWait(20);
    ZT_EQ("после двух Ctrl+Z файл прежний", std::string("- item\n\n  $$z$$\n"), editor.markdown());
    ZT_TRUE("формула на месте после Ctrl+Z",
            editor.objectBlock(zametti::ObjectKind::Formula, QStringLiteral("$$z$$")) >= 0);

    // Ещё раз Ctrl+Enter — и Backspace в пустом пункте: убирается ПУНКТ, а не
    // формула (случай владельца), каретка встаёт на формулу.
    editor.caretToBlock(editor.objectBlock(zametti::ObjectKind::Formula, QStringLiteral("$$z$$")));
    QTest::keyClick(&editor, Qt::Key_Return, Qt::ControlModifier);
    QTest::qWait(20);
    QTest::keyClick(&editor, Qt::Key_Backspace);
    QTest::qWait(20);
    ZT_TRUE("после Backspace формула цела",
            editor.objectBlock(zametti::ObjectKind::Formula, QStringLiteral("$$z$$")) >= 0);
    ZT_TRUE("каретка встала на формулу",
            zametti::objectOf(editor.textCursor().block()).kind == zametti::ObjectKind::Formula);
    ZT_EQ("файл после Backspace — как до Ctrl+Enter", std::string("- item\n\n  $$z$$\n"),
          editor.markdown());
    checkLegal(editor, "после Backspace в пустом пункте");
}

// --- Shift+Enter: продолжить пункт текстом под объектом и под кодом --------

void checkContinueItemAfterObject() {
    struct Case {
        const char* name;
        const char* source;
        zametti::ObjectKind kind;   // None — блок кода
        const char* mark;
        const char* typed;
        const char* expected;
    };
    const Case cases[] = {
        {"формула в пункте", "- item\n\n  $$z$$\n", zametti::ObjectKind::Formula, "$$z$$", "more",
         "- item\n\n  $$z$$\n\n  more\n"},
        {"картинка в пункте", "- item\n\n  ![pic](img.png)\n", zametti::ObjectKind::Image, "pic",
         "more", "- item\n\n  ![pic](img.png)\n\n  more\n"},
        {"таблица в пункте", "- item\n\n  | a | b |\n  |---|---|\n", zametti::ObjectKind::Table,
         "| a | b |", "more", "- item\n\n  | a | b |\n  |---|---|\n\n  more\n"},
        {"формула на втором уровне", "- a\n  - b\n\n    $$q$$\n", zametti::ObjectKind::Formula,
         "$$q$$", "more", "- a\n  - b\n\n    $$q$$\n\n    more\n"},
        {"формула вне списка", "text\n\n$$z$$\n", zametti::ObjectKind::Formula, "$$z$$", "more",
         "text\n\n$$z$$\n\nmore\n"},
        {"блок кода в пункте", "- item\n\n  ```py\n  x = 1\n  ```\n", zametti::ObjectKind::None, "",
         "more", "- item\n\n  ```py\n  x = 1\n  ```\n\n  more\n"},
    };
    int index = 0;
    for (const Case& c : cases) {
        Editor editor;
        editor.openText(QStringLiteral("shift-enter-%1").arg(index++), c.source);
        const int number = c.kind == zametti::ObjectKind::None
                               ? editor.codeBlock()
                               : editor.objectBlock(c.kind, QString::fromUtf8(c.mark));
        ZT_TRUE(std::string(c.name) + ": блок найден", number >= 0);
        if (number < 0) continue;
        editor.caretToBlock(number, /*atEnd=*/c.kind == zametti::ObjectKind::None);
        QTest::keyClick(&editor, Qt::Key_Return, Qt::ShiftModifier);
        QTest::qWait(20);
        QTest::keyClicks(&editor, QString::fromUtf8(c.typed));
        QTest::qWait(20);
        ZT_EQ(std::string(c.name) + ": файл после Shift+Enter и набора", std::string(c.expected),
              editor.markdown());
        checkLegal(editor, std::string(c.name) + ", после Shift+Enter");
        // Объект (или код) на месте.
        if (c.kind != zametti::ObjectKind::None)
            ZT_TRUE(std::string(c.name) + ": объект цел",
                    editor.objectBlock(c.kind, QString::fromUtf8(c.mark)) >= 0);
        // И отменяется до исходного.
        QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier);
        QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier);
        QTest::qWait(20);
        ZT_EQ(std::string(c.name) + ": после двух Ctrl+Z файл прежний", std::string(c.source),
              editor.markdown());
    }
}

// --- Tab / Shift+Tab: уровень объекта -------------------------------------

void checkTabMovesObjects() {
    // Объект вне списка под пунктом второго уровня: Tab доводит его до уровня
    // блока над ним по одному, Shift+Tab — обратно, до выхода из списка.
    {
        Editor editor;
        editor.openText(QStringLiteral("tab-up"), "- a\n  - b\n\n    $$q$$\n\n$$p$$\n");
        const int p = editor.objectBlock(zametti::ObjectKind::Formula, QStringLiteral("$$p$$"));
        ZT_TRUE("формула p найдена", p >= 0);
        if (p < 0) return;
        editor.caretToBlock(p);
        QTest::keyClick(&editor, Qt::Key_Tab);
        QTest::qWait(20);
        ZT_EQ("Tab: формула вошла в список на уровень 0",
              std::string("- a\n  - b\n\n    $$q$$\n\n  $$p$$\n"), editor.markdown());
        QTest::keyClick(&editor, Qt::Key_Tab);
        QTest::qWait(20);
        ZT_EQ("Tab ещё раз: уровень 1 — как у блока над ней",
              std::string("- a\n  - b\n\n    $$q$$\n\n    $$p$$\n"), editor.markdown());
        QTest::keyClick(&editor, Qt::Key_Tab);
        QTest::qWait(20);
        ZT_EQ("третий Tab: глубже блока над ней не бывает",
              std::string("- a\n  - b\n\n    $$q$$\n\n    $$p$$\n"), editor.markdown());
        checkLegal(editor, "после Tab");
        QTest::keyClick(&editor, Qt::Key_Backtab, Qt::ShiftModifier);
        QTest::qWait(20);
        ZT_EQ("Shift+Tab: назад на уровень 0",
              std::string("- a\n  - b\n\n    $$q$$\n\n  $$p$$\n"), editor.markdown());
        QTest::keyClick(&editor, Qt::Key_Backtab, Qt::ShiftModifier);
        QTest::qWait(20);
        ZT_EQ("Shift+Tab: из списка вон",
              std::string("- a\n  - b\n\n    $$q$$\n\n$$p$$\n"), editor.markdown());
        checkLegal(editor, "после Shift+Tab");
        ZT_TRUE("формула цела после всех Tab",
                editor.objectBlock(zametti::ObjectKind::Formula, QStringLiteral("$$p$$")) >= 0);
    }
    // То же для таблицы и картинки: Tab на объекте под пунктом привязывает.
    struct Case {
        const char* name;
        const char* source;
        zametti::ObjectKind kind;
        const char* mark;
        const char* afterTab;
    };
    const Case cases[] = {
        {"таблица", "- a\n\n| a | b |\n|---|---|\n", zametti::ObjectKind::Table, "| a | b |",
         "- a\n\n  | a | b |\n  |---|---|\n"},
        {"картинка", "- a\n\n![pic](img.png)\n", zametti::ObjectKind::Image, "pic",
         "- a\n\n  ![pic](img.png)\n"},
    };
    int index = 0;
    for (const Case& c : cases) {
        Editor editor;
        editor.openText(QStringLiteral("tab-%1").arg(index++), c.source);
        const int number = editor.objectBlock(c.kind, QString::fromUtf8(c.mark));
        ZT_TRUE(std::string(c.name) + ": объект найден", number >= 0);
        if (number < 0) continue;
        editor.caretToBlock(number);
        QTest::keyClick(&editor, Qt::Key_Tab);
        QTest::qWait(20);
        ZT_EQ(std::string(c.name) + ": Tab привязал к пункту", std::string(c.afterTab),
              editor.markdown());
        checkLegal(editor, std::string(c.name) + " после Tab");
        QTest::keyClick(&editor, Qt::Key_Backtab, Qt::ShiftModifier);
        QTest::qWait(20);
        ZT_EQ(std::string(c.name) + ": Shift+Tab отвязал", std::string(c.source),
              editor.markdown());
    }
    // Абзац под объектом уровня 1, сам уровня 0: Tab доводит до 1.
    {
        Editor editor;
        editor.openText(QStringLiteral("tab-paragraph"),
                        "- a\n  - b\n\n    $$q$$\n\n  tail\n");
        QTextBlock tail;
        for (QTextBlock b = editor.document()->firstBlock(); b.isValid(); b = b.next())
            if (b.text() == QStringLiteral("tail")) tail = b;
        ZT_TRUE("хвост найден", tail.isValid());
        if (!tail.isValid()) return;
        editor.caretToBlock(tail.blockNumber());
        QTest::keyClick(&editor, Qt::Key_Tab);
        QTest::qWait(20);
        ZT_EQ("Tab на абзаце под объектом уровня 1 — уровень 1",
              std::string("- a\n  - b\n\n    $$q$$\n\n    tail\n"), editor.markdown());
        checkLegal(editor, "абзац после Tab");
    }
}

// --- Раскрыть и свернуть формулу внутри пункта: уровень цел -----------------

void checkFlipKeepsLevel() {
    Editor editor;
    editor.openText(QStringLiteral("flip"), "- a\n  - b\n\n    $$q$$\n\n    tail\n");
    const int q = editor.objectBlock(zametti::ObjectKind::Formula, QStringLiteral("$$q$$"));
    ZT_TRUE("формула найдена", q >= 0);
    if (q < 0) return;
    editor.caretToBlock(q);
    QTest::keyClick(&editor, Qt::Key_Return);   // раскрыть
    QTest::qWait(20);
    ZT_TRUE("раскрытая формула — на уровне пункта (" + editor.describe(q) + ")",
            zametti::levelOf(editor.document()->findBlockByNumber(q)) == 1);
    ZT_EQ("каретка в начале исходника", n(0), n(editor.textCursor().positionInBlock()));
    ZT_EQ("файл при раскрытой формуле тот же", std::string("- a\n  - b\n\n    $$q$$\n\n    tail\n"),
          editor.markdown());
    QTest::keyClick(&editor, Qt::Key_Escape);   // свернуть
    QTest::qWait(20);
    ZT_TRUE("свёрнутая формула — снова объект на уровне 1",
            editor.objectBlock(zametti::ObjectKind::Formula, QStringLiteral("$$q$$")) == q &&
                zametti::levelOf(editor.document()->findBlockByNumber(q)) == 1);
    checkLegal(editor, "после флипа");
}

// --- Tab на выделении под списком: новый последний пункт -----------------------

void checkTabAttachesRunAsItem() {
    Editor editor;
    editor.openText(QStringLiteral("tab-run"),
                    "- a\n- b\n\ntext one\n\n```py\ncode\n```\n\n$$q$$\n\n- sub\n\nafter\n");
    // Выделяем от «text one» до «after» включительно.
    QTextBlock from;
    QTextBlock to;
    for (QTextBlock b = editor.document()->firstBlock(); b.isValid(); b = b.next()) {
        if (b.text() == QStringLiteral("text one")) from = b;
        if (b.text() == QStringLiteral("after")) to = b;
    }
    ZT_TRUE("границы выделения найдены", from.isValid() && to.isValid());
    if (!from.isValid() || !to.isValid()) return;
    QTextCursor sel(from);
    sel.setPosition(to.position() + to.length() - 1, QTextCursor::KeepAnchor);
    editor.setTextCursor(sel);
    QTest::keyClick(&editor, Qt::Key_Tab);
    QTest::qWait(30);
    // Пустая строка перед новым пунктом остаётся: она была в файле (просторный
    // список — тот же список), а Tab чужих байтов не трогает.
    ZT_EQ("Tab сделал из выделения последний пункт списка",
          std::string("- a\n- b\n\n- text one\n\n  ```py\n  code\n  ```\n\n  $$q$$\n\n  - sub\n\n  after\n"),
          editor.markdown());
    checkLegal(editor, "после Tab на выделении");
    ZT_TRUE("формула в новом пункте — объект на уровне 0",
            zametti::levelOf(editor.document()->findBlockByNumber(
                editor.objectBlock(zametti::ObjectKind::Formula, QStringLiteral("$$q$$")))) == 0);
    QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier);
    QTest::qWait(20);
    ZT_EQ("Ctrl+Z возвращает исходное",
          std::string("- a\n- b\n\ntext one\n\n```py\ncode\n```\n\n$$q$$\n\n- sub\n\nafter\n"),
          editor.markdown());

    // С заголовком внутри пунктом набор не становится (заголовок в пункте не
    // живёт) — работает прежнее правило Tab: годные блоки выделения
    // привязываются к пункту над ними, заголовок остаётся снаружи.
    Editor refuse;
    refuse.openText(QStringLiteral("tab-run-heading"), "- a\n\ntext\n\n## head\n");
    QTextCursor all(refuse.document()->findBlockByNumber(2));
    all.movePosition(QTextCursor::End, QTextCursor::KeepAnchor);
    refuse.setTextCursor(all);
    QTest::keyClick(&refuse, Qt::Key_Tab);
    QTest::qWait(20);
    ZT_EQ("с заголовком в выделении — только привязка абзаца, заголовок цел",
          std::string("- a\n\n  text\n\n## head\n"), refuse.markdown());
}

}  // namespace

static int ztRunSuite(int argc, char** argv) {
    g_dir = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QDir::tempPath();
    QDir().mkpath(g_dir);
    zametti::loadEmbeddedFonts();
    QString error;
    if (!zametti::Formulas::init(&error))
        std::printf("движок формул не поднялся (%s)\n", qPrintable(error));

    checkRoundTripAndLevels();
    checkNewItemAfterObject();
    checkContinueItemAfterObject();
    checkTabMovesObjects();
    checkFlipKeepsLevel();
    checkTabAttachesRunAsItem();
    return zt::report("объекты внутри списков");
}

TEST(ObjectsInLists, All) {
    std::vector<QByteArray> ztArgs{QByteArrayLiteral("objects_in_lists_test")};
    ztArgs.push_back(zt::TestData::outDir(QStringLiteral("objects-in-lists")).toLocal8Bit());
    std::vector<char*> ztArgv;
    for (QByteArray& a : ztArgs) ztArgv.push_back(a.data());
    EXPECT_EQ(0, ztRunSuite(int(ztArgv.size()), ztArgv.data()));
}
