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

#include "keys.h"
#include "test_util.h"
#include "testdata.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTest>
#include <QTextBlock>
#include <QTextEdit>
#include <QTextCursor>
#include <QTextLayout>
#include <QImage>

#include <cmath>
#include <cstdio>
#include <cstdlib>
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

// ПЛАШКА ПОД ПЕРЕНЕСЁННЫМИ СТРОКАМИ (нашёл владелец): длинная строка кода в
// узком окне ложится в две-три визуальные строки, и полосу получала только
// первая — остальные шли на подложке обычного текста. Спрашивается пикселем на
// второй визуальной строке у правого края колонки, а не числом полос: число
// можно набрать и неверно.
void checkCodePlateWrapped() {
    const QString longLine =
        QStringLiteral("int veryLongIdentifierNumberOne = anotherVeryLongIdentifier + "
                       "yetAnotherLongIdentifier * theLastLongIdentifierOfThisLine;");
    const QString path = writeNote(
        QStringLiteral("плашка-перенос.md"),
        QStringLiteral("текст\n\n```cpp\n") + longLine + QStringLiteral("\n```\n\nхвост\n"));
    Rig rig;
    rig.view.resize(420, 400);
    rig.editor.openFile(path);
    QTest::qWait(20);
    ZT_TRUE("вошли", rig.controller.enter());
    QTest::qWait(40);

    const QTextBlock code = rig.view.document()->findBlockByNumber(3);
    ZT_EQ("это та самая строка кода", longLine.toStdString(), code.text().toStdString());
    const int lines = code.layout() != nullptr ? code.layout()->lineCount() : 0;
    ZT_TRUE("строка кода перенесена (визуальных строк больше одной)", lines > 1);
    if (lines < 2) return;

    const QImage shot = rig.view.viewport()->grab().toImage();
    const QColor page = zametti::settings().style().pageBackground();
    const auto close = [](const QColor& x, const QColor& y) {
        return std::abs(x.red() - y.red()) <= 2 && std::abs(x.green() - y.green()) <= 2 &&
               std::abs(x.blue() - y.blue()) <= 2;
    };
    // Первая визуальная строка плашку имела всегда (набор checkCodePlate) — она
    // и есть образец цвета; остальные обязаны совпасть с ней и отличаться от
    // страницы. Точный состав плашки не вычисляем: смешение у Qt своё.
    QColor sample;
    for (int i = 0; i < lines; ++i) {
        const QTextLine line = code.layout()->lineAt(i);
        // Геометрия визуальной строки — через публичный cursorRect её начала
        // (cursorRect конца у перенесённой строки отдаёт уже начало следующей).
        // Сразу за естественной шириной текста букв нет: там либо плашка, либо
        // страница.
        QTextCursor head(rig.view.document());
        head.setPosition(code.position() + line.textStart());
        const QRect headRect = rig.view.cursorRect(head);
        const int x = qMin(shot.width() - 2, int(headRect.left() + line.naturalTextWidth()) + 3);
        const QColor pixel = shot.pixelColor(QPoint(x, headRect.center().y()));
        if (i == 0) {
            sample = pixel;
            ZT_TRUE("первая визуальная строка блока кода лежит на плашке (не на странице)",
                    !close(pixel, page));
            continue;
        }
        ZT_TRUE("визуальная строка " + std::to_string(i) + " блока кода лежит на той же плашке",
                close(pixel, sample) && !close(pixel, page));
    }
    // ТОЧКИ У ПЕРЕНЕСЁННЫХ СТРОК (просьба владельца): на левом поле, у начала
    // каждой визуальной строки, кроме первой, — точка цвета комментариев;
    // у первой строки — нет. Смотрим снимок всего вида: поле — вне вьюпорта.
    {
        const QImage whole = rig.view.grab().toImage();
        const QRect vp = rig.view.viewport()->geometry();
        const QColor dot = zametti::settings().markdownHighlighting().comment();
        const auto dotAt = [&](int yInViewport) {
            const int y = vp.top() + yInViewport;
            for (int x = qMax(0, vp.left() - 40); x < vp.left(); ++x) {
                const QColor c = whole.pixelColor(x, y);
                // Сглаженный край светлее точки; спрашиваем «заметно темнее страницы».
                if (c.lightness() < (page.lightness() + dot.lightness()) / 2) return true;
            }
            return false;
        };
        for (int i = 0; i < lines; ++i) {
            QTextCursor head(rig.view.document());
            head.setPosition(code.position() + code.layout()->lineAt(i).textStart());
            const QRect headRect = rig.view.cursorRect(head);
            // Точка — на половине высоты строчной буквы: чуть выше середины строки
            // проверяем столбик в несколько пикселей.
            bool found = false;
            for (int dy = -3; dy <= 3 && !found; ++dy) found = dotAt(headRect.center().y() + dy);
            if (i == 0)
                ZT_TRUE("у первой визуальной строки точки на поле нет", !found);
            else
                ZT_TRUE("у перенесённой строки " + std::to_string(i) + " точка на поле", found);
        }
        // И у обычной строки («хвост») её нет.
        const QTextBlock tail = rig.view.document()->findBlockByNumber(7);
        QTextCursor at(rig.view.document());
        at.setPosition(tail.position());
        bool found = false;
        for (int dy = -3; dy <= 3 && !found; ++dy) found = dotAt(rig.view.cursorRect(at).center().y() + dy);
        ZT_TRUE("у обычной строки точки нет", !found);
    }

    rig.view.grab().save(QDir(g_dir).filePath(QStringLiteral("плашка-кода-перенос.png")));
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


// --- КЛАВИШИ РЕЖИМА: ТАБЛИЦА КРАЁВ ------------------------------------------
//
// Enter продолжает пункт (и выходит из списка на пустом), иначе держит отступ;
// Shift+Enter продолжает пункт строкой содержимого; Tab/Shift+Tab двигают
// пункт под соседа / на родителя, вне списка — пробелы до стопа; Ctrl+D
// (toggleTaskKey) переключает задачи строк. Каждый случай — одно нажатие, и
// ОДИН undo обязан вернуть исходный текст: правило = один шаг отмены.
//
// Текст случая — «|» это каретка (ровно одна), «[» и «]» — концы выделения
// (тогда «|» не нужна: каретка в «]», якорь в «[»).
struct KeyCase {
    const char* before;   // с «|» или «[…]»
    const char* key;      // "Enter", "Shift+Enter", "Tab", "Shift+Tab", "Task"
    const char* after;    // с «|» — где каретка должна оказаться
    const char* what;
};

const KeyCase kKeyCases[] = {
    // Enter на пункте
    {"- foo|", "Enter", "- foo\n- |", "буллет продолжается буллетом"},
    {"- fo|o", "Enter", "- fo\n- |o", "хвост уезжает в новый пункт"},
    {"- |foo", "Enter", "- \n- |foo", "каретка в начале содержимого — пустой пункт над"},
    {"-| foo", "Enter", "-\n| foo", "каретка внутри маркера — простой перенос"},
    {"|- foo", "Enter", "\n|- foo", "каретка в начале строки — строка уезжает"},
    {"- |", "Enter", "|", "пустой пункт — из списка вон"},
    {"  - |", "Enter", "|", "пустой вложенный пункт — строка пустеет целиком"},
    {"- [ ] |", "Enter", "|", "пустая задача — тоже"},
    {"- [x] done|", "Enter", "- [x] done\n- [ ] |", "задача продолжается незакрытой"},
    {"-[x] done|", "Enter", "-[x] done\n- [ ] |", "краткая задача — продолжение каноничное"},
    {"* [ ] a|", "Enter", "* [ ] a\n* [ ] |", "звёздочка остаётся звёздочкой"},
    {"1. a|", "Enter", "1. a\n2. |", "номер растёт"},
    {"10. a|", "Enter", "10. a\n11. |", "двузначный номер"},
    {"7) a|", "Enter", "7) a\n8) |", "скобка остаётся скобкой"},
    {"  - a|", "Enter", "  - a\n  - |", "отступ пробелами копируется"},
    {"\t- a|", "Enter", "\t- a\n\t- |", "отступ табом копируется дословно"},
    {"- a\n  * b|", "Enter", "- a\n  * b\n  * |", "свой знак буллета, не родительский"},
    {"-|", "Enter", "-\n|", "дефис без пробела — не пункт"},
    // Enter вне пункта
    {"    code|", "Enter", "    code\n    |", "автоотступ"},
    {"  |  - a", "Enter", "  \n  |  - a", "каретка в отступе — отступ обрезан по каретке"},
    {"```\n- a|\n```", "Enter", "```\n- a\n|\n```", "в заборе пункт — код: простой перенос"},
    {"- a\n  ```\n  int x;|\n  ```", "Enter", "- a\n  ```\n  int x;\n  |\n  ```", "в заборе — автоотступ"},
    {"- fo[o\n- b]ar", "Enter", "- fo\n- |ar", "выделение снимается, правило по получившейся строке"},
    {"# T|", "Enter", "# T\n|", "заголовок — обычный перенос"},
    // Shift+Enter
    {"- foo|", "Shift+Enter", "- foo\n  |", "продолжение пункта под содержимым"},
    {"- fo|o", "Shift+Enter", "- fo\n  |o", "хвост уезжает на строку содержимого"},
    {"10. foo|", "Shift+Enter", "10. foo\n    |", "колонка содержимого номера — 4"},
    {"- [ ] foo|", "Shift+Enter", "- [ ] foo\n  |", "у задачи — 2, чекбокс не в счёт"},
    {"- foo\n  bar|", "Shift+Enter", "- foo\n  bar\n  |", "продолжение продолжения"},
    {"- a\n  - b\n    c|", "Shift+Enter", "- a\n  - b\n    c\n    |", "продолжение вложенного"},
    {"para|", "Shift+Enter", "para\n|", "вне списка — как Enter"},
    {"- a\n\npara|", "Shift+Enter", "- a\n\npara\n|", "абзац после списка — список кончился"},
    {"|- foo", "Shift+Enter", "\n|- foo", "перед маркером — простой перенос"},
    // Tab
    {"- a\n- b|", "Tab", "- a\n  - b|", "пункт уходит под соседа"},
    {"- a|", "Tab", "- a|", "первый пункт — некуда"},
    {"- a\n  - b|", "Tab", "- a\n  - b|", "первый ребёнок — некуда"},
    {"- a\n  - a1\n- b|", "Tab", "- a\n  - a1\n  - b|", "более глубокие пропускаются"},
    {"- a\n  - a1\n  - a2|", "Tab", "- a\n  - a1\n    - a2|", "вложенный под вложенного"},
    {"1. a\n2. b|", "Tab", "1. a\n   2. b|", "под номер — три пробела"},
    {"- [ ] a\n- [ ] b|", "Tab", "- [ ] a\n  - [ ] b|", "под задачу — два, не шесть"},
    {"\t- a\n\t- b|", "Tab", "\t- a\n\t  - b|", "таб соседа — дословно, плюс два пробела"},
    {"- a\n\n- b|", "Tab", "- a\n\n  - b|", "пустая строка между — не помеха"},
    {"- a\n  text\n- b|", "Tab", "- a\n  text\n  - b|", "содержимое пункта выше пропускается"},
    {"- a\n\npara\n\n- b|", "Tab", "- a\n\npara\n\n- b|", "абзац между — списки разные, некуда"},
    {"- a\n- |b", "Tab", "- a\n  - |b", "каретка на знаке содержимого остаётся на нём"},
    {"|- a\n- b", "Tab", "|- a\n- b", "первый пункт с кареткой в начале — некуда"},
    {"foo|", "Tab", "foo |", "вне списка — пробелы до стопа (колонка 3 → 4)"},
    {"fo|", "Tab", "fo  |", "до стопа — два пробела с колонки 2"},
    {"```\n- a|\n```", "Tab", "```\n- a |\n```", "в заборе пункт — код, пробелы до стопа"},
    // Shift+Tab
    {"- a\n  - b|", "Shift+Tab", "- a\n- b|", "на отступ родителя"},
    {"- a\n  - b\n    - c|", "Shift+Tab", "- a\n  - b\n  - c|", "на отступ родителя, не в ноль"},
    {"- b|", "Shift+Tab", "- b|", "нулевой отступ — ничего"},
    {"  - b|", "Shift+Tab", "- b|", "родителя нет — в ноль"},
    {"    foo|", "Shift+Tab", "foo|", "вне списка — снять до стопа"},
    // Выделение в несколько строк: единый сдвиг по первой строке
    {"- a\n[- b\n  - c]", "Tab", "- a\n[  - b\n    - c]", "выделение: дельта первой строки-пункта всем"},
    {"[foo\nbar]", "Tab", "[    foo\n    bar]", "выделение вне списка — по стопу"},
    {"[    foo\n\n    bar]", "Shift+Tab", "[foo\n\nbar]", "выделение: пустая строка не трогается"},
    // Ctrl+D
    {"- [ ] a|", "Task", "- [x] a|", "задача отмечается"},
    {"- [x] a|", "Task", "- [ ] a|", "и снимается"},
    {"- [X] a|", "Task", "- [ ] a|", "заглавная тоже снимается"},
    {"-[ ] a|", "Task", "-[x] a|", "краткая запись сохраняется"},
    {"- a|", "Task", "- a|", "буллет — ничего"},
    {"para|", "Task", "para|", "абзац — ничего"},
    {"1. [ ] a|", "Task", "1. [ ] a|", "чекбокс у номера — текст, ничего"},
    {"[- [ ] a\n- [x] b\n- c\n- [ ] d]", "Task", "[- [x] a\n- [x] b\n- c\n- [x] d]", "выделение: первая задача задаёт направление"},
    {"[para\n- [x] b\n- [ ] c]", "Task", "[para\n- [ ] b\n- [ ] c]", "направление от первой ЗАДАЧИ, не первой строки"},
    {"```\n- [ ] a|\n```", "Task", "```\n- [ ] a|\n```", "в заборе — код, ничего"},
};

// Разбор «|» / «[…]»: возвращает чистый текст и позиции.
struct Marked {
    QString text;
    int anchor = -1;
    int caret = -1;
};

// Выделение размечается иначе, чтобы не путаться с чекбоксами: «[» и «]» стоят
// ТОЛЬКО в случаях, где чекбоксов в этих местах нет; распознаём так: если в
// строке есть «|» — выделения нет; иначе первая «[» — якорь, последняя «]» —
// каретка.
Marked markedOf(const char* raw) {
    const QString src = QString::fromUtf8(raw);
    Marked m;
    if (src.contains(QLatin1Char('|'))) {
        for (const QChar c : src) {
            if (c == QLatin1Char('|')) { m.caret = int(m.text.size()); continue; }
            m.text.append(c);
        }
        return m;
    }
    const int open = int(src.indexOf(QLatin1Char('[')));
    const int close = int(src.lastIndexOf(QLatin1Char(']')));
    for (int i = 0; i < src.size(); ++i) {
        if (i == open) { m.anchor = int(m.text.size()); continue; }
        if (i == close) { m.caret = int(m.text.size()); continue; }
        m.text.append(src.at(i));
    }
    return m;
}

void checkKeyCases() {
    const QString path = writeNote(QStringLiteral("клавиши.md"), QStringLiteral("x\n"));
    Rig rig;
    rig.editor.openFile(path);
    QTest::qWait(20);
    ZT_TRUE("вошли", rig.controller.enter());

    for (const KeyCase& c : kKeyCases) {
        const Marked before = markedOf(c.before);
        const Marked after = markedOf(c.after);
        const std::string what = std::string(c.what) + " [" + c.key + " на «" + c.before + "»]";
        rig.view.showSource(before.text, {0, 0, true});
        QCoreApplication::processEvents();   // подсветчик — состояния заборов
        QTextCursor at(rig.view.document());
        at.setPosition(before.anchor >= 0 ? before.anchor : before.caret);
        if (before.anchor >= 0) at.setPosition(before.caret, QTextCursor::KeepAnchor);
        rig.view.setTextCursor(at);

        const std::string key = c.key;
        if (key == "Enter") QTest::keyClick(&rig.view, Qt::Key_Return);
        else if (key == "Shift+Enter") QTest::keyClick(&rig.view, Qt::Key_Return, Qt::ShiftModifier);
        else if (key == "Tab") QTest::keyClick(&rig.view, Qt::Key_Tab);
        else if (key == "Shift+Tab") QTest::keyClick(&rig.view, Qt::Key_Backtab);
        else if (key == "Task") {
            ZT_TRUE(what + ": у переключения задачи есть сочетание",
                    zt::pressKey(&rig.view, zametti::settings().editor().toggleTaskKey()));
        }
        QCoreApplication::processEvents();

        ZT_EQ(what + ": текст", after.text.toStdString(), rig.view.source().toStdString());
        const QTextCursor got = rig.view.textCursor();
        ZT_EQ(what + ": каретка", std::to_string(after.caret), std::to_string(got.position()));
        if (after.anchor >= 0)
            ZT_EQ(what + ": якорь", std::to_string(after.anchor), std::to_string(got.anchor()));

        // Один шаг отмены — или нуль шагов, если ничего не менялось.
        if (after.text != before.text) {
            rig.view.undo();
            ZT_EQ(what + ": один undo вернул текст", before.text.toStdString(),
                  rig.view.source().toStdString());
        } else {
            ZT_TRUE(what + ": ничего не менялось — и отменять нечего",
                    !rig.view.document()->isUndoAvailable());
        }
    }
    rig.controller.leave();
}


// КАРЕТКА В ИСХОДНИКЕ — ТОГО ЖЕ ЦВЕТА И ТОЛЩИНЫ, что в обычном виде (просьба
// владельца). Снимок вьюпорта сразу после движения каретки (она мигает —
// берём сразу после wake): в cursorRect() пиксели цвета caretColor ровно на
// толщину caretWidth × масштаб.
void checkCaretLook() {
    const QString path = writeNote(QStringLiteral("каретка.md"),
                                   QStringLiteral("давайте напишем немного текста\n"));
    Rig rig;
    rig.editor.openFile(path);
    QTest::qWait(20);
    ZT_TRUE("вошли", rig.controller.enter());
    rig.view.setFocus();
    QTest::qWait(20);
    ZT_TRUE("фокус у вида исходника", rig.view.hasFocus());

    QTextCursor at(rig.view.document());
    at.setPosition(5);
    rig.view.setTextCursor(at);
    QCoreApplication::processEvents();

    const QColor caret = zametti::settings().style().caretColor();
    const int want = qMax(1, qRound(zametti::settings().style().caretWidth() * rig.view.zoom()));
    const QRect r = rig.view.cursorRect();
    const QImage shot = rig.view.viewport()->grab().toImage();
    const int y = r.center().y();
    int run = 0;
    for (int x = r.left(); x < shot.width() && shot.pixelColor(x, y) == caret; ++x) ++run;
    ZT_EQ("каретка цвета caretColor и толщиной caretWidth", std::to_string(want),
          std::to_string(run));
    ZT_TRUE("левее каретки — не её цвет", r.left() == 0 || shot.pixelColor(r.left() - 1, y) != caret);
    // Та же мера в обычном виде — чтобы сравнение было не с числом, а с ним.
    QTextCursor editorAt(rig.editor.document());
    editorAt.setPosition(rig.editor.document()->firstBlock().position() + 5);
    rig.controller.leave();
    rig.editor.activateWindow();
    rig.editor.setFocus();
    QTest::qWait(20);
    rig.editor.setTextCursor(editorAt);
    QCoreApplication::processEvents();
    ZT_TRUE("фокус у редактора", rig.editor.hasFocus());
    const QRect er = rig.editor.cursorRect();
    const QImage eshot = rig.editor.viewport()->grab().toImage();
    int erun = 0;
    for (int x = er.left(); x < eshot.width() && eshot.pixelColor(x, er.center().y()) == caret; ++x) ++erun;
    ZT_EQ("в обычном виде каретка той же толщины", std::to_string(want), std::to_string(erun));
}


// ДНО СТЕКА ОТМЕНЫ В РЕЖИМЕ (нашёл владелец): Ctrl+Z, когда в тексте отменять
// больше нечего, не должен быть тупиком — режим закрывается (наложить нечего:
// всё отменено), и отмена уходит заметке, а с её дна — в историю, как в
// обычном виде. Один ряд Ctrl+Z: правки исходника → правки заметки → история.
void checkUndoAtBottomLeavesToHistory() {
    const QString path = writeNote(QStringLiteral("дно.md"), QStringLiteral("раз\n\nдва\n"));
    Rig rig;
    rig.editor.openFile(path);
    QTest::qWait(20);
    int historyAsked = 0;
    QObject::connect(&rig.editor, &zametti::NoteEditor::historyRequested, &rig.editor,
                     [&historyAsked] { ++historyAsked; });
    ZT_TRUE("вошли", rig.controller.enter());

    QTextCursor edit = rig.view.textCursor();
    edit.movePosition(QTextCursor::End);
    edit.insertText(QStringLiteral("три"));
    QTest::keyClick(&rig.view, Qt::Key_Z, Qt::ControlModifier);
    QTest::qWait(10);
    ZT_EQ("первый Ctrl+Z отменил правку текста", std::string("раз\n\nдва\n"),
          rig.view.source().toStdString());
    ZT_TRUE("режим ещё идёт", rig.controller.active());
    ZT_EQ("в историю пока не просились", std::string("0"), std::to_string(historyAsked));

    QTest::keyClick(&rig.view, Qt::Key_Z, Qt::ControlModifier);
    QTest::qWait(10);
    ZT_TRUE("на дне стека режим закрылся", !rig.controller.active());
    ZT_EQ("заметка цела", std::string("раз\n\nдва\n"), textOf(rig.editor));
    ZT_EQ("и отмена ушла заметке — у свежей заметки это просьба об истории",
          std::string("1"), std::to_string(historyAsked));
}


// НЕРАЗРЫВНЫЕ ПРОБЕЛЫ В НАЧАЛЕ СТРОК ПЕРЕЖИВАЮТ РЕЖИМ (нашёл владелец:
// стихотворение с отступами U+00A0 теряло их на выходе из исходника, хотя
// внешний редактор их сохранял). QTextDocument::toPlainText() подменяет U+00A0
// обычным пробелом — а обычный пробел в начале строки markdown съедает.
void checkNbspSurvives() {
    const QString body = QStringLiteral("   стих\n   второй\n\nобычный\n");
    const QString path = writeNote(QStringLiteral("нбсп.md"), body);
    Rig rig;
    rig.editor.openFile(path);
    QTest::qWait(20);
    ZT_TRUE("вошли", rig.controller.enter());
    ZT_TRUE("вид показывает неразрывные как есть", rig.view.source().contains(QChar(0xa0)));
    // Правка в другом месте — чтобы наложение вообще состоялось.
    QTextCursor edit = rig.view.textCursor();
    edit.movePosition(QTextCursor::End);
    edit.insertText(QStringLiteral("хвост\n"));
    ZT_TRUE("вышли, наложив", rig.controller.leave() > 0);
    ZT_EQ("неразрывные отступы целы, хвост на месте",
          (QStringLiteral("   стих\n   второй\n\nобычный\nхвост\n")).toStdString(),
          textOf(rig.editor));
}

// ESC НЕ ВЫВОДИТ ИЗ РЕЖИМА ИСХОДНИКА (решение владельца, refactor3): выход
// накладывает всю набранную работу на заметку, и случайное нажатие клавиши под
// рукой для этого не годится — выходят кнопкой [M]. У правки настроек Esc
// остался: там выход безвреден.
void checkEscapeKeepsMode() {
    const QString path = writeNote(QStringLiteral("эскейп.md"), QStringLiteral("раз\n"));
    Rig rig;
    rig.editor.openFile(path);
    QTest::qWait(20);
    ZT_TRUE("вошли", rig.controller.enter());
    QTextCursor edit = rig.view.textCursor();
    edit.movePosition(QTextCursor::End);
    edit.insertText(QStringLiteral("\nдва\n"));

    QTest::keyClick(&rig.view, Qt::Key_Escape);
    QTest::qWait(10);
    ZT_TRUE("режим идёт после Esc", rig.controller.active());
    ZT_TRUE("и набранное на месте", rig.view.source().contains(QStringLiteral("два")));

    // Выход — кнопкой (её зовёт окно тем же глаголом).
    ZT_TRUE("кнопка вывела", rig.controller.leave() > 0);
    ZT_TRUE("режим кончился", !rig.controller.active());
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
    checkCodePlateWrapped();
    checkKeyCases();
    checkCaretLook();
    checkUndoAtBottomLeavesToHistory();
    checkNbspSurvives();
    checkEscapeKeepsMode();
    checkOwnerNote();
}
