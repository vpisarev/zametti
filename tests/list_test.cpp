// Редактирование списков — целиком, от клавиши до файла.
//
// Списки в заметках правят чаще всего остального, и ошибка здесь дороже любой
// другой. Проверка идёт живым набором в настоящем редакторе, а не вызовом
// операций: одна и та же правка операции может быть верной, а через клавишу
// давать не то — курсор оказывается не там, обработчик зовёт не ту операцию,
// пересборка сбивает место. Ровно так и вышло с Enter в начале пункта: сама
// операция была разумной, а вставку пункта между двумя она сломала.
//
// Каждый случай — это: заметка, куда встать, что нажать, что должно выйти и где
// оказаться курсору. Проверяется и содержимое, и место курсора: половина
// правил про списки — это как раз про то, куда он встаёт.

#include "doc_model.h"
#include "document_reader.h"
#include "editor_widget.h"
#include "serializer.h"
#include "settings.h"

#include "test_util.h"

#include <QApplication>
#include <QKeySequence>
#include <QScrollBar>
#include <QTest>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

fs::path g_dir;

QString writeNote(const std::string& name, const QString& text) {
    const fs::path path = g_dir / name;
    std::ofstream out(path, std::ios::binary);
    const QByteArray bytes = text.toUtf8();
    out.write(bytes.constData(), bytes.size());
    return QString::fromStdString(path.string());
}

QString textOf(const zametti::NoteEditor& editor) {
    return QString::fromStdString(
        zametti::serialize(zametti::readDocument(*editor.document())));
}

// Нажатие в виде «что человек делает»: сочетание клавиш или набор текста.
struct Press {
    const char* keys;    // как в настройках: "Return", "Ctrl+3", "Shift+Tab"
    const char* typed;   // либо просто набрать это
};

Press key(const char* keys) { return {keys, nullptr}; }
Press type(const char* text) { return {nullptr, text}; }

struct Case {
    const char* what;
    const char* source;
    int block;            // куда встать: номер блока
    int offset;           // и смещение в нём
    std::vector<Press> presses;
    const char* expected;
    int cursorBlock;      // в каком блоке обязан оказаться курсор; -1 — не проверяем
};

void run(const Case& c) {
    const QString path = writeNote(std::string(c.what) + ".md", QString::fromUtf8(c.source));

    zametti::NoteEditor editor;
    editor.resize(700, 500);
    editor.show();
    QTest::qWait(20);
    editor.setFocus();
    editor.openFile(path);
    QTest::qWait(20);

    QTextCursor cursor = editor.textCursor();
    const QTextBlock start = editor.document()->findBlockByNumber(c.block);
    if (!start.isValid()) {
        ZT_TRUE(std::string(c.what) + ": блока " + std::to_string(c.block) + " нет", false);
        return;
    }
    cursor.setPosition(start.position() + qMin(c.offset, start.length() - 1));
    editor.setTextCursor(cursor);

    for (const Press& press : c.presses) {
        if (press.typed != nullptr) {
            editor.insertPlainText(QString::fromUtf8(press.typed));
        } else {
            const QKeySequence sequence(QString::fromLatin1(press.keys),
                                        QKeySequence::PortableText);
            const QKeyCombination combo = sequence[0];
            QTest::keyClick(&editor, combo.key(), combo.keyboardModifiers());
        }
        QTest::qWait(10);
    }

    ZT_EQ(c.what, std::string(c.expected), textOf(editor).toStdString());
    if (c.cursorBlock >= 0)
        ZT_EQ(std::string(c.what) + ": курсор в блоке", std::to_string(c.cursorBlock),
              std::to_string(editor.textCursor().blockNumber()));
}

// Вставка и выход. Здесь важнее всего место курсора: печатать человек будет
// сразу после нажатия, и если курсор не там, правка уедет не туда.
const Case kEnterCases[] = {
    {"пункт между двумя",
     "- раз\n- два\n- три\n",
     1, 0, {key("Return"), type("новый")},
     "- раз\n- новый\n- два\n- три\n", 1},

    {"пункт перед первым",
     "- раз\n- два\n",
     0, 0, {key("Return"), type("ноль")},
     "- ноль\n- раз\n- два\n", 0},

    {"пункт после текущего",
     "- раз\n- два\n",
     0, 5, {key("Return"), type("полтора")},
     "- раз\n- полтора\n- два\n", 1},

    {"разрез пункта посередине",
     "- разъдва\n",
     0, 4, {key("Return")},
     "- разъ\n- два\n", 1},

    {"выход из списка в конце",
     "- раз\n- два\n",
     1, 5, {key("Return"), key("Return"), type("абзац")},
     "- раз\n- два\n\nабзац\n", 2},

    // Два Enter в начале пункта разрывают список: пункт выходит из него и
    // становится абзацем, а абзац списки и разделяет. Пустой абзац для этого не
    // годится — пустая строка между пунктами не разделяет ничего, и разрыв
    // пропадал бы при первом же сохранении.
    {"разрыв списка посередине",
     "- раз\n- два\n- три\n",
     1, 0, {key("Return"), key("Return")},
     "- раз\n\nдва\n\n- три\n", 1},

    {"разрыв держится после записи",
     "- раз\n- задачки:\n- [ ] дело\n",
     1, 0, {key("Return"), key("Return"), key("Ctrl+S")},
     "- раз\n\nзадачки:\n\n- [ ] дело\n", 1},

    {"новая задача невыполненная",
     "- [x] сделано\n",
     0, 8, {key("Return"), type("ещё")},
     "- [x] сделано\n- [ ] ещё\n", 1},

    {"вложенный пункт продолжается вложенным",
     "- верх\n  - вложенный\n",
     1, 11, {key("Return"), type("сосед")},
     "- верх\n  - вложенный\n  - сосед\n", 2},
};

// Слияние и снятие списка.
const Case kBackspaceCases[] = {
    {"пункт сливается с предыдущим",
     "- раз\n- два\n",
     1, 0, {key("Backspace")},
     "- раздва\n", 0},

    {"первый пункт перестаёт быть пунктом",
     "- раз\n- два\n",
     0, 0, {key("Backspace")},
     "раз\n\n- два\n", 0},

    {"пустой пункт исчезает",
     "- раз\n- \n",
     1, 0, {key("Backspace")},
     "- раз\n", 0},
};

// Уровни. Поддерево едет вместе с родителем — это правило легко сломать.
const Case kIndentCases[] = {
    {"отступ пункта",
     "- раз\n- два\n",
     1, 0, {key("Tab")},
     "- раз\n  - два\n", 1},

    {"первый пункт отступать некуда",
     "- раз\n- два\n",
     0, 0, {key("Tab")},
     "- раз\n- два\n", 0},

    {"поддерево едет вместе с пунктом",
     "- раз\n- два\n  - внук\n",
     1, 0, {key("Tab")},
     "- раз\n  - два\n    - внук\n", 1},

    {"выступ возвращает на уровень",
     "- раз\n  - два\n",
     1, 0, {key("Shift+Tab")},
     "- раз\n- два\n", 1},

    {"выступ тянет поддерево",
     "- раз\n  - два\n    - внук\n",
     1, 0, {key("Shift+Tab")},
     "- раз\n- два\n  - внук\n", 1},
};

// Превращения. Нумерация обязана пересчитаться сама.
const Case kKindCases[] = {
    // Пустых строк между прогонами канон не ставит: маркеры и так разные.
    {"буллет в нумерованный",
     "- раз\n- два\n- три\n",
     1, 0, {key("Ctrl+3")},
     "- раз\n1. два\n- три\n", 1},

    {"весь список в нумерованный",
     "- раз\n- два\n- три\n",
     0, 0, {key("Ctrl+A"), key("Ctrl+3")},
     "1. раз\n2. два\n3. три\n", -1},

    {"весь список в задачи",
     "- раз\n- два\n",
     0, 0, {key("Ctrl+A"), key("Ctrl+T")},
     "- [ ] раз\n- [ ] два\n", -1},

    {"задачи в буллеты — отметка исчезает",
     "- [x] раз\n- [ ] два\n",
     0, 0, {key("Ctrl+A"), key("Ctrl+8")},
     "- раз\n- два\n", -1},

    {"пункт в абзац разлепляет списки",
     "- раз\n- задачки:\n- [ ] дело\n",
     1, 0, {key("Ctrl+Shift+0")},
     "- раз\n\nзадачки:\n\n- [ ] дело\n", 1},
};

// Перестановка. Пункт едет со своим поддеревом, нумерация пересчитывается.
const Case kMoveCases[] = {
    {"пункт вниз",
     "- раз\n- два\n- три\n",
     0, 3, {key("Ctrl+Down")},
     "- два\n- раз\n- три\n", 1},

    {"пункт вверх",
     "- раз\n- два\n- три\n",
     2, 3, {key("Ctrl+Up")},
     "- раз\n- три\n- два\n", 1},

    {"первый пункт вверх не едет",
     "- раз\n- два\n",
     0, 3, {key("Ctrl+Up")},
     "- раз\n- два\n", 0},

    {"последний вниз не едет",
     "- раз\n- два\n",
     1, 3, {key("Ctrl+Down")},
     "- раз\n- два\n", 1},

    {"пункт едет с поддеревом",
     "- раз\n  - внук\n- два\n",
     0, 3, {key("Ctrl+Down")},
     "- два\n- раз\n  - внук\n", 1},

    {"нумерация пересчитывается",
     "1. раз\n2. два\n3. три\n",
     0, 4, {key("Ctrl+Down")},
     "1. два\n2. раз\n3. три\n", 1},
};

// Автозамена при наборе — с ней списки и заводят.
const Case kInputCases[] = {
    {"дефис и пробел заводят список",
     "текст\n",
     0, 5, {key("Return"), key("Return"), type("-"), key("Space"), type("пункт")},
     "текст\n\n- пункт\n", 1},

    {"звёздочка тоже",
     "текст\n",
     0, 5, {key("Return"), key("Return"), type("*"), key("Space"), type("пункт")},
     "текст\n\n- пункт\n", 1},

    {"номер заводит нумерованный",
     "текст\n",
     0, 5, {key("Return"), key("Return"), type("1."), key("Space"), type("пункт")},
     "текст\n\n1. пункт\n", 1},

    {"дефис со скобкой заводит задачу",
     "текст\n",
     0, 5, {key("Return"), key("Return"), type("-["), key("Space"), type("дело")},
     "текст\n\n- [ ] дело\n", 1},

    {"правило работает и на второй строке абзаца",
     "вступление\n",
     0, 10, {key("Return"), type("-"), key("Space"), type("пункт")},
     "вступление\n\n- пункт\n", 1},
};

// Отметка задач.
const Case kTaskCases[] = {
    {"переключить задачу",
     "- [ ] дело\n",
     0, 6, {key("Ctrl+Space")},
     "- [x] дело\n", 0},

    {"переключить обратно",
     "- [x] дело\n",
     0, 6, {key("Ctrl+Space")},
     "- [ ] дело\n", 0},

    {"на буллете молчит",
     "- пункт\n",
     0, 3, {key("Ctrl+Space")},
     "- пункт\n", 0},

    {"выделенные задачи переключаются разом",
     "- [ ] раз\n- [ ] два\n- [ ] три\n",
     0, 0, {key("Ctrl+A"), key("Ctrl+Space")},
     "- [x] раз\n- [x] два\n- [x] три\n", -1},

    {"хоть одна выполненная — снимаются все",
     "- [x] раз\n- [ ] два\n",
     0, 0, {key("Ctrl+A"), key("Ctrl+Space")},
     "- [ ] раз\n- [ ] два\n", -1},
};

// Отмена: каждая правка списка обязана откатываться целиком.
void checkUndo() {
    struct Undoable {
        const char* what;
        const char* source;
        int block;
        int offset;
        const char* keys;
    };
    const Undoable cases[] = {
        {"Enter в начале", "- раз\n- два\n", 1, 0, "Return"},
        {"Backspace у маркера", "- раз\n- два\n", 1, 0, "Backspace"},
        {"отступ", "- раз\n- два\n", 1, 0, "Tab"},
        {"превращение", "- раз\n- два\n", 1, 0, "Ctrl+3"},
        {"перестановка", "- раз\n- два\n", 0, 3, "Ctrl+Down"},
        {"переключение задачи", "- [ ] дело\n", 0, 6, "Ctrl+Space"},
    };
    for (const Undoable& c : cases) {
        const QString path =
            writeNote(std::string("отмена-") + c.what + ".md", QString::fromUtf8(c.source));

        zametti::NoteEditor editor;
        editor.resize(700, 500);
        editor.show();
        QTest::qWait(20);
        editor.setFocus();
        editor.openFile(path);
        QTest::qWait(20);

        QTextCursor cursor = editor.textCursor();
        const QTextBlock start = editor.document()->findBlockByNumber(c.block);
        cursor.setPosition(start.position() + qMin(c.offset, start.length() - 1));
        editor.setTextCursor(cursor);

        const QKeySequence sequence(QString::fromLatin1(c.keys), QKeySequence::PortableText);
        QTest::keyClick(&editor, sequence[0].key(), sequence[0].keyboardModifiers());
        QTest::qWait(10);
        QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier);
        QTest::qWait(10);

        ZT_EQ(std::string("отмена вернула документ: ") + c.what, std::string(c.source),
              textOf(editor).toStdString());
    }
}

// Ритм страницы вокруг списка. Он намеренно несимметричен: список идёт вплотную
// под вводной строкой — так его и набирают, строка, Enter, "- " — а после списка
// стоит заметный воздух, как пустая строка в самом файле.
//
// Проверка нужна ровно потому, что однажды это уже «выровняли»: одна отбивка на
// все стыки убирала прыжок пункта, ставшего абзацем, но вид от этого стал явно
// хуже — огромный зазор между вводной строкой и первым пунктом.
void checkListRhythm() {
    const QString path = writeNote(
        "ритм.md",
        QStringLiteral("вводная строка:\n\n- первый\n- второй\n\nабзац после\n\n"
                       "ещё абзац\n"));

    zametti::NoteEditor editor;
    editor.resize(700, 400);
    editor.show();
    QTest::qWait(20);
    editor.setFocus();
    editor.openFile(path);
    QTest::qWait(20);

    auto marginOf = [&editor](int number) {
        return editor.document()->findBlockByNumber(number).blockFormat().topMargin();
    };

    ZT_TRUE("список идёт вплотную под вводной строкой",
            marginOf(1) < marginOf(4));
    ZT_TRUE("внутри списка отбивки нет", marginOf(2) <= 0.01);
    ZT_TRUE("после списка воздуха больше, чем между абзацами",
            marginOf(3) > marginOf(4));
    ZT_TRUE("между абзацами отбивка есть", marginOf(4) > 0.01);
}

}  // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    g_dir = fs::temp_directory_path() / "zametti-list-test";
    fs::remove_all(g_dir);
    fs::create_directories(g_dir);

    for (const Case& c : kEnterCases) run(c);
    for (const Case& c : kBackspaceCases) run(c);
    for (const Case& c : kIndentCases) run(c);
    for (const Case& c : kKindCases) run(c);
    for (const Case& c : kMoveCases) run(c);
    for (const Case& c : kInputCases) run(c);
    for (const Case& c : kTaskCases) run(c);
    checkUndo();
    checkListRhythm();

    fs::remove_all(g_dir);
    return zt::report("списки");
}
