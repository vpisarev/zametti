// Буфер обмена: копирование, вырезание, вставка.
//
// Обменный формат — сам markdown, отдельного MIME-типа нет. Идемпотентность
// ядра и есть гарантия того, что скопированное внутри приложения вставится без
// потерь, а вставленное в чужой редактор окажется валидным markdown.

#include "doc_model.h"
#include "document_reader.h"
#include "editor_widget.h"
#include "parser.h"
#include "serializer.h"
#include "settings.h"
#include "test_util.h"

#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QFile>
#include <QTest>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>

#include <string>

namespace {

QString g_dir;

void checkEqual(const QString& expected, const QString& actual, const std::string& what) {
    ++zt::g_checks;
    if (expected == actual) return;
    ++zt::g_failures;
    std::printf("провал: %s\n  ждали:  %s\n  вышло:  %s\n", what.c_str(),
                expected.toUtf8().replace("\n", "\\n").constData(),
                actual.toUtf8().replace("\n", "\\n").constData());
}

QString writeNote(const char* name, const QString& text) {
    const QString path = g_dir + QLatin1Char('/') + QLatin1String(name);
    QFile file(path);
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) file.write(text.toUtf8());
    return path;
}

QString textOf(const zametti::NoteEditor& editor) {
    return QString::fromStdString(
        zametti::serialize(zametti::readDocument(*editor.document())));
}

// Выделение по блокам и смещениям в них: так случай читается глазами.
void select(zametti::NoteEditor& editor, int fromBlock, int fromOffset, int toBlock,
            int toOffset) {
    QTextCursor cursor = editor.textCursor();
    cursor.setPosition(editor.document()->findBlockByNumber(fromBlock).position() + fromOffset);
    cursor.setPosition(editor.document()->findBlockByNumber(toBlock).position() + toOffset,
                       QTextCursor::KeepAnchor);
    editor.setTextCursor(cursor);
}

struct Editor {
    zametti::NoteEditor widget;
    QString path;

    explicit Editor(const char* name, const QString& content) {
        path = writeNote(name, content);
        widget.resize(700, 500);
        widget.show();
        QTest::qWait(20);
        widget.setFocus();
        widget.openFile(path);
        QTest::qWait(20);
    }
};

// Копирование: что оказывается в буфере.
struct CopyCase {
    const char* note;
    int fromBlock;
    int fromOffset;
    int toBlock;
    int toOffset;
    const char* clipboard;
    const char* what;
};

void checkCopy(const CopyCase& c, int index) {
    Editor editor((std::string("copy") + std::to_string(index) + ".md").c_str(),
                  QString::fromUtf8(c.note));
    select(editor.widget, c.fromBlock, c.fromOffset, c.toBlock, c.toOffset);
    editor.widget.copy();
    QTest::qWait(10);
    checkEqual(QString::fromUtf8(c.clipboard), QGuiApplication::clipboard()->text(), c.what);
}

const CopyCase kCopyCases[] = {
    {"- раз\n- два\n- три\n", 0, 0, 2, 3, "- раз\n- два\n- три\n",
     "три пункта копируются списком"},
    {"- раз\n- два\n", 0, 2, 1, 2, "- з\n- дв\n",
     "частично выделенные крайние пункты остаются пунктами"},
    {"- верх\n  - вложенный\n", 1, 0, 1, 9, "- вложенный\n",
     "вложенный пункт копируется с ребейзом уровня"},
    {"абзац с **жирным** внутри\n", 0, 8, 0, 14, "**жирным**",
     "кусок абзаца копируется без завершающего перевода"},
    // Пустая строка между блоками — сама блок, поэтому абзац здесь второй, а не
    // первый.
    {"# заголовок\n\nабзац\n", 0, 0, 2, 5, "# заголовок\n\nабзац\n",
     "заголовок и абзац"},
    {"```py\nодна\nдве\n```\n", 0, 0, 1, 3, "```py\nодна\nдве\n```\n",
     "две строки блока кода остаются одним блоком"},
    {"- [x] дело\n- [ ] другое\n", 0, 0, 1, 6, "- [x] дело\n- [ ] другое\n",
     "задачи сохраняют отметки"},
    {"| a | b |\n|---|---|\n| 1 | 2 |\n", 0, 0, 2, 9,
     "| a | b |\n|---|---|\n| 1 | 2 |\n", "дословный кусок копируется как есть"},
};

// Копирование всего документа побайтово равно сериализации — требование брифа.
void checkCopyAll() {
    const QString source =
        QStringLiteral("# заголовок\n\n- раз\n- [x] два\n\n```py\nx = 1\n```\n\nхвост\n");
    Editor editor("copyall.md", source);
    editor.widget.selectAll();
    editor.widget.copy();
    QTest::qWait(10);
    checkEqual(source, QGuiApplication::clipboard()->text(),
               "копирование всего документа равно сериализации");
}

// Вставка текста X в пустую заметку эквивалентна открытию файла X.
void checkPasteEqualsOpen() {
    const QString source =
        QStringLiteral("# заголовок\n\n- раз\n  - вложенный\n\n```py\nx = 1\n```\n");
    Editor pasted("paste-empty.md", QString());
    QGuiApplication::clipboard()->setText(source);
    pasted.widget.paste();
    QTest::qWait(20);
    checkEqual(source, textOf(pasted.widget),
               "вставка в пустую заметку равна открытию такого файла");
}

// Копировать, вырезать, вставить на то же место — документ обязан вернуться.
void checkRoundTrip() {
    const QString source = QStringLiteral("- раз\n- два\n- три\n");
    Editor editor("roundtrip.md", source);

    select(editor.widget, 1, 0, 2, 0);
    editor.widget.cut();
    QTest::qWait(20);
    checkEqual(QStringLiteral("- раз\n- три\n"), textOf(editor.widget),
               "вырезание убирает пункт");

    editor.widget.paste();
    QTest::qWait(20);
    checkEqual(source, textOf(editor.widget), "вставка возвращает его на место");

    editor.widget.undo();
    QTest::qWait(10);
    checkEqual(QStringLiteral("- раз\n- три\n"), textOf(editor.widget),
               "вставка отменяется одним шагом");
}

// Вставка одиночного абзаца — инлайновая: слова входят в текущий блок, а не
// заводят новый.
void checkInlinePaste() {
    Editor editor("inline.md", QStringLiteral("- пункт\n"));
    QGuiApplication::clipboard()->setText(QStringLiteral("**вставка**"));

    QTextCursor cursor = editor.widget.textCursor();
    cursor.movePosition(QTextCursor::EndOfBlock);
    editor.widget.setTextCursor(cursor);
    editor.widget.paste();
    QTest::qWait(20);

    checkEqual(QStringLiteral("- пункт**вставка**\n"), textOf(editor.widget),
               "одиночный абзац входит в текущий пункт");
}

// Ctrl+Shift+V: буфер входит как текст, разметка не разбирается.
void checkLiteralPaste() {
    Editor editor("literal.md", QStringLiteral("текст\n"));
    QGuiApplication::clipboard()->setText(QStringLiteral("- не список"));

    QTextCursor cursor = editor.widget.textCursor();
    cursor.movePosition(QTextCursor::End);
    editor.widget.setTextCursor(cursor);
    QTest::keyClick(&editor.widget, Qt::Key_V, Qt::ControlModifier | Qt::ShiftModifier);
    QTest::qWait(20);

    // В конце строки дефис экранировать не от чего: списком он был бы только в
    // начале блока.
    checkEqual(QStringLiteral("текст- не список\n"), textOf(editor.widget),
               "литеральная вставка не заводит список");
}

// В блок кода markdown не вставляется: там текст буквальный.
void checkPasteIntoCode() {
    Editor editor("code.md", QStringLiteral("```py\nx = 1\n```\n"));
    QGuiApplication::clipboard()->setText(QStringLiteral("- не список"));

    QTextCursor cursor = editor.widget.textCursor();
    cursor.movePosition(QTextCursor::End);
    editor.widget.setTextCursor(cursor);
    editor.widget.paste();
    QTest::qWait(20);

    checkEqual(QStringLiteral("```py\nx = 1- не список\n```\n"), textOf(editor.widget),
               "в блоке кода вставленное остаётся текстом");
}

}  // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    if (argc < 2) {
        std::printf("использование: clipboard_test <каталог для временных файлов>\n");
        return 2;
    }

    g_dir = QString::fromLocal8Bit(argv[1]) + QStringLiteral("/clipboard-data");
    QDir(g_dir).removeRecursively();
    if (!QDir().mkpath(g_dir)) {
        std::printf("не создать каталог %s\n", g_dir.toUtf8().constData());
        return 2;
    }

    int index = 0;
    for (const CopyCase& c : kCopyCases) checkCopy(c, index++);
    checkCopyAll();
    checkPasteEqualsOpen();
    checkRoundTrip();
    checkInlinePaste();
    checkLiteralPaste();
    checkPasteIntoCode();

    std::printf("проверок %d, провалов %d\n", zt::g_checks, zt::g_failures);
    return zt::g_failures == 0 ? 0 : 1;
}
