// РЕЖИМ ПРАВКИ ИСХОДНИКА ЦЕЛИКОМ: вид, контроллер, редактор и заметка вместе.
//
// Ядро проверено без виджетов (SourceApply, SourceCaret). Здесь спрашивается
// то, чего в ядре нет: круг «вошли — поправили текст — вышли», место каретки на
// обоих переходах, свой буфер отмены режима, Tab пробелами и — главное —
// РЕЖИМ ПЕРЕЖИВАЕТ СМЕНУ ЗАМЕТКИ (просьба владельца: по заметкам ходят, кнопка
// [M] остаётся нажатой).

#include "editor_widget.h"
#include "markdown_controller.h"
#include "markdown_edit_view.h"
#include "pieces.h"

#include "test_util.h"
#include "testdata.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QTest>
#include <QTextBlock>
#include <QTextEdit>
#include <QTextCursor>

#include <cstdio>
#include <string>

namespace {

QString g_dir;

QString writeNote(const QString& name, const QString& text) {
    const QString path = QDir(g_dir).filePath(name);
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return {};
    f.write(text.toUtf8());
    f.close();
    return path;
}

std::string textOf(zametti::NoteEditor& editor) {
    return markdownOf(blocksOf(*editor.document()));
}

// Живое окно режима: редактор, вид исходника и контроллер между ними.
struct Rig {
    zametti::NoteEditor editor;
    zametti::MarkdownEditView view;
    zametti::MarkdownController controller;

    Rig() : controller(editor, view) {
        editor.resize(700, 500);
        editor.show();
        view.resize(700, 500);
        view.show();
        QTest::qWait(20);
    }
};

// Вошли, поправили слово, вышли: заметка приняла правку, каретка на месте, а
// отмена возвращает всё ОДНИМ нажатием.
void checkRoundTrip() {
    const QString path = writeNote(QStringLiteral("круг.md"),
                                   QStringLiteral("# Заголовок\n\nпервый абзац\n\nвторой абзац\n"));
    Rig rig;
    rig.editor.openFile(path);
    QTest::qWait(20);

    // Каретка во втором абзаце — на неё и смотрим при переходе.
    QTextCursor at = rig.editor.textCursor();
    const QTextBlock target = rig.editor.document()->findBlockByNumber(4);
    at.setPosition(target.position() + 3);
    rig.editor.setTextCursor(at);

    ZT_TRUE("вошли в режим", rig.controller.enter());
    ZT_TRUE("режим идёт", rig.controller.active());
    ZT_EQ("вид показывает исходник целиком",
          std::string("# Заголовок\n\nпервый абзац\n\nвторой абзац\n"),
          rig.view.source().toStdString());
    ZT_EQ("каретка на той же строке исходника", std::string("4"),
          std::to_string(rig.view.caretPos().line));
    ZT_EQ("и в той же колонке", std::string("3"),
          std::to_string(rig.view.caretPos().column));

    // Правим текст как текст.
    QTextCursor edit = rig.view.textCursor();
    edit.setPosition(rig.view.document()->findBlockByNumber(4).position());
    edit.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
    edit.insertText(QStringLiteral("ВТОРОЙ абзац"));
    rig.view.setTextCursor(edit);

    ZT_EQ("вышли, наложив один кусок", std::string("1"), std::to_string(rig.controller.leave()));
    ZT_TRUE("режим кончился", !rig.controller.active());
    ZT_EQ("заметка приняла правку",
          std::string("# Заголовок\n\nпервый абзац\n\nВТОРОЙ абзац\n"), textOf(rig.editor));
    ZT_EQ("каретка вернулась в тот же блок", std::string("4"),
          std::to_string(rig.editor.textCursor().block().blockNumber()));

    // ОДНИМ НАЖАТИЕМ. Вся правка исходника — один шаг отмены заметки.
    rig.editor.undo();
    QTest::qWait(10);
    ZT_EQ("Ctrl+Z вернул заметку целиком",
          std::string("# Заголовок\n\nпервый абзац\n\nвторой абзац\n"), textOf(rig.editor));
}

// Свой буфер отмены: пока идёт режим, Ctrl+Z отменяет правку ТЕКСТА.
void checkOwnUndo() {
    const QString path = writeNote(QStringLiteral("отмена.md"), QStringLiteral("раз\n\nдва\n"));
    Rig rig;
    rig.editor.openFile(path);
    QTest::qWait(20);
    ZT_TRUE("вошли", rig.controller.enter());

    QTextCursor edit = rig.view.textCursor();
    edit.movePosition(QTextCursor::End);
    edit.insertText(QStringLiteral("\nтри\n"));
    ZT_TRUE("текст стал другим", rig.view.source().contains(QStringLiteral("три")));
    rig.view.undo();
    ZT_TRUE("отмена режима вернула текст", !rig.view.source().contains(QStringLiteral("три")));
    ZT_EQ("а заметка при этом не менялась", std::string("раз\n\nдва\n"), textOf(rig.editor));

    ZT_EQ("выход без правок — ноль кусков", std::string("0"),
          std::to_string(rig.controller.leave()));
}

// Пока идёт режим, заметка правку не принимает: истина живёт в тексте.
void checkNoteRefusesEdits() {
    const QString path = writeNote(QStringLiteral("запрет.md"), QStringLiteral("раз\n\nдва\n"));
    Rig rig;
    rig.editor.openFile(path);
    QTest::qWait(20);
    ZT_TRUE("вошли", rig.controller.enter());

    QTextCursor at = rig.editor.textCursor();
    at.setPosition(0);
    ZT_TRUE("глагол правки отказал", !rig.editor.note().insertText(at, QStringLiteral("х")));
    ZT_EQ("и заметка цела", std::string("раз\n\nдва\n"), textOf(rig.editor));
    rig.controller.leave();
    ZT_TRUE("а после выхода — принимает",
            rig.editor.note().insertText(at, QStringLiteral("х")));
}

// Tab заполняет пробелами до стопа, а не ставит знак табуляции.
void checkTabIsSpaces() {
    const QString path = writeNote(QStringLiteral("табы.md"), QStringLiteral("раз\n"));
    Rig rig;
    rig.editor.openFile(path);
    QTest::qWait(20);
    ZT_TRUE("вошли", rig.controller.enter());

    QTextCursor at = rig.view.textCursor();
    at.setPosition(0);
    rig.view.setTextCursor(at);
    QTest::keyClick(&rig.view, Qt::Key_Tab);
    QTest::qWait(10);
    const int stop = zametti::settings().editor().codeTabWidth();
    ZT_EQ("отступ пробелами до стопа", std::string(size_t(stop), ' ') + "раз",
          rig.view.document()->findBlockByNumber(0).text().toStdString());
    ZT_TRUE("знака табуляции в тексте нет", !rig.view.source().contains(QLatin1Char('\t')));

    QTest::keyClick(&rig.view, Qt::Key_Backtab);
    QTest::qWait(10);
    ZT_EQ("Shift+Tab снял его обратно", std::string("раз"),
          rig.view.document()->findBlockByNumber(0).text().toStdString());
    rig.controller.leave();
}

// РЕЖИМ ПЕРЕЖИВАЕТ СМЕНУ ЗАМЕТКИ, а правки прежней при этом не теряются.
void checkSurvivesNoteChange() {
    const QString first = writeNote(QStringLiteral("первая.md"), QStringLiteral("первая\n"));
    const QString second = writeNote(QStringLiteral("вторая.md"), QStringLiteral("вторая\n"));
    Rig rig;
    rig.editor.openFile(first);
    QTest::qWait(20);
    ZT_TRUE("вошли", rig.controller.enter());

    QTextCursor edit = rig.view.textCursor();
    edit.movePosition(QTextCursor::End);
    edit.insertText(QStringLiteral("\nдописано\n"));

    rig.editor.openFile(second);
    QTest::qWait(20);
    ZT_TRUE("режим не погас", rig.controller.active());
    ZT_EQ("вид показывает новую заметку", std::string("вторая\n"),
          rig.view.source().toStdString());

    // Вернулись — правка прежней на месте, и она в файле.
    rig.editor.openFile(first);
    QTest::qWait(20);
    ZT_EQ("правка прежней заметки не потерялась", std::string("первая\n\nдописано\n"),
          rig.view.source().toStdString());
    rig.controller.leave();
}

// Шапку в исходнике съедать нельзя: текст не принят, режим не закрывается.
void checkHeaderRefused() {
    const QString path = writeNote(QStringLiteral("шапка.md"), QStringLiteral("раз\n"));
    Rig rig;
    rig.editor.openFile(path);
    QTest::qWait(20);
    ZT_TRUE("вошли", rig.controller.enter());

    rig.view.setPlainText(QStringLiteral("<!-- zametti\nparent: x\n-->\n\nраз\n"));
    ZT_EQ("текст отвергнут", std::string("-1"), std::to_string(rig.controller.leave()));
    ZT_TRUE("и режим не закрылся", rig.controller.active());
    ZT_EQ("заметка цела", std::string("раз\n"), textOf(rig.editor));
}

// ПЛАШКА ПОД БЛОКОМ КОДА и подложка под кодом в строке (просьба владельца:
// «блоки кода и inline код выводить на светло-сером фоне»). Спрашивается у
// самих подсветок: сколько полос во всю ширину положено под строки забора.
void checkCodePlate() {
    const QString path = writeNote(
        QStringLiteral("плашка.md"),
        QStringLiteral("текст с `кодом в строке`\n\n```cpp\nint a = 1;\nint b = 2;\n```\n\n"
                       "хвост\n"));
    Rig rig;
    rig.view.resize(900, 600);
    rig.editor.openFile(path);
    QTest::qWait(20);
    ZT_TRUE("вошли", rig.controller.enter());
    QTest::qWait(40);

    int bands = 0;
    for (const QTextEdit::ExtraSelection& one : rig.view.extraSelections())
        if (one.format.boolProperty(QTextFormat::FullWidthSelection)) ++bands;
    // Четыре строки: открывающий забор, две строки кода, закрывающий.
    ZT_EQ("полоса под каждой строкой блока кода", std::string("4"), std::to_string(bands));

    rig.view.grab().save(QDir(g_dir).filePath(QStringLiteral("плашка-кода.png")));
    rig.controller.leave();
}

// МАСШТАБ РЕЖИМА — СВОЙ, и это проверяется в обе стороны: клавиши в исходнике
// не трогают обычный вид, клавиши в обычном виде не трогают исходник.
void checkOwnZoom() {
    const QString path = writeNote(QStringLiteral("масштаб.md"), QStringLiteral("раз\n"));
    Rig rig;
    rig.editor.openFile(path);
    QTest::qWait(20);

    const qreal noteZoom = rig.editor.zoom();
    const qreal sourceZoom = rig.view.zoom();
    ZT_TRUE("вошли", rig.controller.enter());

    rig.view.applyZoom(2.0);
    QTest::qWait(10);
    ZT_TRUE("исходник увеличился", rig.view.zoom() > sourceZoom);
    ZT_TRUE("а заметка осталась как была", qFuzzyCompare(rig.editor.zoom(), noteZoom));
    // И кегль настоящий, а не только число.
    ZT_TRUE("кегль исходника вырос",
            rig.view.font().pointSizeF() >
                zametti::settings().style().baseFontPoint() * 1.5);

    rig.controller.leave();
    rig.editor.applyZoom(1.5);
    QTest::qWait(10);
    ZT_TRUE("заметка увеличилась", rig.editor.zoom() > noteZoom);
    ZT_TRUE("а исходник остался со своим", qFuzzyCompare(rig.view.zoom(), 2.0));
}

// ЗАМЕТКА ВЛАДЕЛЬЦА, А НЕ ВЫДУМАННЫЙ ДИСТИЛЛЯТ. Берётся ЖИВАЯ копия, а не
// архивная: у архивной в файле лежит один заголовок-стаб, тело живёт в журнале,
// и «проверка на настоящей заметке» свелась бы к одной строке — первый снимок
// именно это и показал. «Typesetting Math in Markdown» —
// формулы выключные и строчные, картинки, таблицы, блоки кода. Спрашивается
// главное: исходник, показанный человеку, и есть тело заметки байт в байт, а
// возврат БЕЗ ПРАВОК не трогает её вовсе (ноль кусков) — иначе режим тихо
// переписывал бы заметку на каждом заходе.
//
// Здесь же — приёмочные снимки в узком и широком окне: у агента окно узкое, у
// владельца широкое, и три бага этого класса проект уже ловил.
void checkOwnerNote() {
    const QString source =
        zt::TestData::root() + QStringLiteral("/owner-copy/01n7wcv5fkf6ne.md");
    if (!QFile::exists(source)) {
        std::printf("owner-copy: корпуса нет, акт пропущен\n");
        return;
    }
    const QString path = QDir(g_dir).filePath(QStringLiteral("формулы.md"));
    QFile::remove(path);
    if (!QFile::copy(source, path)) {
        std::printf("owner-copy: не скопировалось\n");
        return;
    }
    QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner);

    for (const int width : {700, 1600}) {
        Rig rig;
        rig.view.resize(width, 900);
        rig.editor.resize(width, 900);
        QTest::qWait(20);
        rig.editor.openFile(path);
        QTest::qWait(60);
        const std::string was = textOf(rig.editor);

        ZT_TRUE("вошли в режим на заметке владельца (" + std::to_string(width) + ")",
                rig.controller.enter());
        QTest::qWait(40);
        // «С ТОЧНОСТЬЮ ДО CR», а не побайтово, и это не поблажка: в этой самой
        // заметке 46 CRLF внутри исходников формул, а плоский виджет их не
        // держит — человек их не видит и набрать не может. Здесь же
        // проверяется, что от этого они НЕ ПРОПАДАЮТ (см. ниже): нетронутый
        // блок не перекладывается вовсе.
        QString shown = rig.view.source();
        QString body = QString::fromStdString(was);
        ZT_EQ("исходник и есть тело заметки (" + std::to_string(width) + ")",
              body.remove(QLatin1Char('\r')).toStdString(),
              shown.remove(QLatin1Char('\r')).toStdString());
        ZT_TRUE("в заметке есть CRLF — есть чему теряться (" + std::to_string(width) + ")",
                was.find('\r') != std::string::npos);

        rig.view.grab().save(QDir(g_dir).filePath(
            QStringLiteral("исходник-%1.png").arg(width)));

        ZT_EQ("возврат без правок не трогает заметку (" + std::to_string(width) + ")",
              std::string("0"), std::to_string(rig.controller.leave()));
        ZT_EQ("и тело цело до байта, вместе с CRLF (" + std::to_string(width) + ")", was,
              textOf(rig.editor));
    }
    std::printf("снимки режима: %s\n", g_dir.toUtf8().constData());
}

}  // namespace

TEST(MarkdownEdit, All) {
    g_dir = zt::TestData::outDir(QStringLiteral("markdown-edit"));
    checkRoundTrip();
    checkOwnUndo();
    checkNoteRefusesEdits();
    checkTabIsSpaces();
    checkSurvivesNoteChange();
    checkHeaderRefused();
    checkOwnZoom();
    checkCodePlate();
    checkOwnerNote();
}
