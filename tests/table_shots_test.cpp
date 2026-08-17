// Снимки и протокол таблиц-объектов: то, что владелец проверяет глазами.
//
// Таблица — объект (сессия 5): один знак U+FFFC, сетка поверх полосы во всю
// колонку, правка — флип объект ⇄ исходник (Enter / двойной щелчок раскрывают,
// Esc / уход каретки сворачивают судьёй файла). Здесь проверяется проводка к
// редактору и то, что видно на снимке: каждая колонка нарисована, выбранная
// таблица показана уголками, каретка внутри не рисуется, файл от показа не
// меняется, геометрия после ухода-возврата та же, что при свежем открытии.
#include "block_object.h"
#include "doc_model.h"
#include "document_builder.h"
#include "editor_widget.h"
#include "note_view.h"
#include "pieces.h"
#include "settings.h"
#include "table_object.h"
#include "test_util.h"
#include "testdata.h"

#include <QAbstractTextDocumentLayout>
#include <QApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QScrollBar>
#include <QTest>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>

#include <string>
#include <vector>

namespace {
QString g_dir;

std::string n(int v) { return std::to_string(v); }

const char* kNote = R"(# Таблицы

Обычный абзац перед таблицей.

| деталь | цена | наличие |
|:---|---:|:---:|
| болт | 10 | да |
| гайка с длинным именем | 1000 | нет |
| шайба | 5 | да |

Абзац между таблицами.

| что | зачем |
|---|---|
| **жир** | видно ли |
| `код` | и он тоже |
| [ссылка](https://example.com) | цветом |

| колонка | очень длинное описание |
|---|---|
| раз | очень длинное описание детали, которое ни в какую колонку целиком не поместится и обязано перенестись по словам, а не раздуть таблицу |
| два | коротко |

Хвост заметки.
)";

// Блоки-таблицы документа (объекты), по порядку.
QVector<int> tableBlocks(const zametti::NoteView& view) {
    QVector<int> out;
    for (QTextBlock b = view.document()->firstBlock(); b.isValid(); b = b.next())
        if (zametti::isTableObjectBlock(b)) out.push_back(b.blockNumber());
    return out;
}

// Раскрытая таблица: дословный блок с текстом исходника, объектом не является.
bool isOpenedTable(const QTextBlock& b) {
    return zametti::isRawBlock(b) && !zametti::isTableObjectBlock(b) &&
           zametti::looksLikeTable(zametti::sourceTextOf(b));
}

std::string markdown(const zametti::NoteView& view) { return markdownOf(blocksOf(*view.document())); }

class Editor : public zametti::NoteEditor {
public:
    void openText(const QString& name, const char* text, int width = 900, int height = 700) {
        const QString path = QDir(g_dir).filePath(name + QStringLiteral(".md"));
        QFile file(path);
        if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) file.write(text);
        file.close();
        resize(width, height);
        show();
        QTest::qWait(20);
        openFile(path);
        QTest::qWait(60);
    }
    QPoint viewportPoint(const QPointF& documentPoint) {
        return QPoint(int(documentPoint.x()) - horizontalScrollBar()->value(),
                      int(documentPoint.y()) - verticalScrollBar()->value());
    }
};

// В КАЖДОЙ КОЛОНКЕ ЧТО-ТО НАРИСОВАНО.
//
// Проверка появилась после того, как моя же оптимизация раскладки унесла текст
// колонок с выравниванием вправо и по центру на километр за экран: ширину
// текста я спрашивал у boundingRect() раскладки, а там стояла ширина строки —
// бесконечная. Поймал снимок, глазами. Теперь ловит набор. Считаем тёмные
// точки ВНУТРИ РЯДОВ, отступя от их границ, — иначе линии сетки, идущие через
// все колонки, делали проверку пустышкой.
void checkEveryColumnDrawn(zametti::NoteEditor& editor, const QImage& shot, const std::string& tag) {
    const QVector<int> tables = tableBlocks(editor);
    ZT_TRUE(tag + ": таблицы показаны объектами", !tables.isEmpty());
    if (tables.isEmpty()) return;
    const QTextBlock block = editor.document()->findBlockByNumber(tables.first());
    const zametti::TableRender* table = editor.tableRenderFor(block);
    ZT_TRUE(tag + ": у первой таблицы есть раскладка", table != nullptr && table->layout.rows > 0);
    if (table == nullptr) return;
    const QRectF area = editor.tableRect(tables.first());
    const int scroll = editor.verticalScrollBar()->value();
    // Снимок — виджета целиком, а прямоугольник — в координатах документа:
    // между ними вьюпорт, сдвинутый полями колонки.
    const QPoint origin = editor.viewport()->mapTo(&editor, QPoint(0, 0));
    const qreal dpr = shot.devicePixelRatio();

    qreal x = area.left();
    for (int column = 0; column < table->layout.columns; ++column) {
        const qreal width = table->layout.columnWidth.at(column);
        int dark = 0;
        qreal y = area.top();
        for (int row = 0; row < table->layout.rows; ++row) {
            const qreal height = table->layout.rowHeight.at(row);
            for (int px = int(x) + 2; px < int(x + width) - 2; ++px)
                for (int py = int(y - scroll) + 4; py < int(y + height - scroll) - 4; ++py) {
                    const int sx = int((px + origin.x()) * dpr);
                    const int sy = int((py + origin.y()) * dpr);
                    if (sx < 0 || sy < 0 || sx >= shot.width() || sy >= shot.height()) continue;
                    if (qGray(shot.pixel(sx, sy)) < 128) ++dark;
                }
            y += height;
        }
        ZT_TRUE(tag + ": в колонке " + n(column) + " что-то нарисовано", dark > 0);
        x += width;
    }
}

void shoot(const QString& name, int width, int height, const char* text) {
    Editor editor;
    editor.openText(name, text, width, height);
    QTextCursor at = editor.textCursor();
    at.setPosition(0);
    editor.setTextCursor(at);
    QTest::qWait(60);
    const QImage shot = editor.grab().toImage();
    if (!shot.save(QDir(g_dir).filePath(name + QStringLiteral(".png"))))
        std::printf("НЕ СОХРАНИЛСЯ снимок %s\n", qPrintable(name));
    checkEveryColumnDrawn(editor, shot, name.toStdString());
    ZT_EQ(name.toStdString() + ": в заметке три таблицы-объекта", n(3), n(tableBlocks(editor).size()));
    // Полоса каждой таблицы не ниже сетки, сетка внутри полосы.
    for (const int number : tableBlocks(editor)) {
        const QTextBlock b = editor.document()->findBlockByNumber(number);
        const QRectF band = editor.document()->documentLayout()->blockBoundingRect(b);
        const QRectF grid = editor.tableRect(number);
        ZT_TRUE(name.toStdString() + ": сетка блока " + n(number) + " внутри полосы (низ " +
                    n(int(grid.bottom())) + ", полоса до " + n(int(band.bottom())) + ")",
                !grid.isEmpty() && grid.bottom() <= band.bottom() + 1.0 && grid.top() >= band.top() - 1.0);
    }
}

// --- флип: Enter раскрывает, уход каретки сворачивает --------------------------

void checkFlip() {
    Editor editor;
    editor.openText(QStringLiteral("флип"), "до\n\n| a | b |\n|---|---|\n| 1 | 2 |\n\nпосле\n");
    const QVector<int> tables = tableBlocks(editor);
    ZT_TRUE("таблица показана объектом", tables.size() == 1);
    if (tables.size() != 1) return;
    const int number = tables.first();
    const int blocks = editor.document()->blockCount();
    const std::string before = markdown(editor);

    // Каретка на таблицу — и Enter раскрывает исходник в том же блоке.
    editor.setTextCursor(QTextCursor(editor.document()->findBlockByNumber(number)));
    QTest::qWait(20);
    QTest::keyClick(&editor, Qt::Key_Return);
    QTest::qWait(40);
    const QTextBlock opened = editor.document()->findBlockByNumber(number);
    ZT_TRUE("Enter раскрыл таблицу дословным блоком", isOpenedTable(opened));
    ZT_EQ("исходник раскрытой таблицы", std::string("| a | b |\n|---|---|\n| 1 | 2 |"),
          zametti::sourceTextOf(opened).toStdString());
    ZT_EQ("блоков столько же (флип 1 ↔ 1)", n(blocks), n(editor.document()->blockCount()));
    ZT_EQ("каретка в начале исходника", n(0), n(editor.textCursor().positionInBlock()));
    ZT_EQ("файл от раскрытия не изменился", before, markdown(editor));
    ZT_TRUE("раскрытая таблица не объект", !zametti::objectOf(opened).valid());
    // И у неё есть высота — все строки видны.
    ZT_TRUE("раскрытая таблица размечена в три строки",
            editor.document()->documentLayout()->blockBoundingRect(opened).height() >
                2 * QFontMetricsF(editor.baseFont()).height());

    // Увели каретку наружу — снова объект.
    editor.setTextCursor(QTextCursor(editor.document()->firstBlock()));
    QTest::qWait(60);
    ZT_TRUE("уход каретки вернул объект", zametti::isTableObjectBlock(editor.document()->findBlockByNumber(number)));
    ZT_EQ("файл после сворачивания тот же", before, markdown(editor));
    ZT_TRUE("сетка снова считается", editor.tableRenderFor(editor.document()->findBlockByNumber(number)) != nullptr);
}

// --- мышь: щелчок выбирает, двойной — раскрывает в ячейку ----------------------

void checkMouse() {
    Editor editor;
    editor.openText(QStringLiteral("мышь"), "до\n\n| a | b |\n|---|---|\n| 1 | 2 |\n| 3 | 4 |\n\nпосле\n");
    const QVector<int> tables = tableBlocks(editor);
    ZT_TRUE("таблица есть", tables.size() == 1);
    if (tables.size() != 1) return;
    const int number = tables.first();
    const QRectF area = editor.tableRect(number);
    ZT_TRUE("сетка размечена", !area.isEmpty());
    const QPoint middle = editor.viewportPoint(area.center());

    // Одинарный щелчок по сетке — таблица выбрана.
    QTest::mouseClick(editor.viewport(), Qt::LeftButton, Qt::NoModifier, middle);
    QTest::qWait(30);
    ZT_TRUE("щелчок по сетке выбрал таблицу (каретка в блоке " + n(editor.textCursor().blockNumber()) + ")",
            zametti::objectOf(editor.textCursor().block()).kind == zametti::ObjectKind::Table);

    // Выбранная таблица показана уголками-мишенями — тем же, чем фотография.
    {
        const QImage shot = editor.grab().toImage();
        const QColor caretColour = zametti::settings().style().caretColor();
        const QPoint origin = editor.viewport()->mapTo(&editor, QPoint(0, 0));
        const int scroll = editor.verticalScrollBar()->value();
        int cornerPixels = 0;
        for (int px = int(area.left()) - 12; px < int(area.right()) + 12; ++px)
            for (int py = int(area.top()) - 12; py < int(area.bottom()) + 12; ++py) {
                if (area.contains(QPointF(px, py))) continue;
                const int sx = px + origin.x();
                const int sy = py - scroll + origin.y();
                if (sx < 0 || sy < 0 || sx >= shot.width() || sy >= shot.height()) continue;
                const QColor at = shot.pixelColor(sx, sy);
                if (qAbs(at.red() - caretColour.red()) < 20 &&
                    qAbs(at.green() - caretColour.green()) < 20 &&
                    qAbs(at.blue() - caretColour.blue()) < 20)
                    ++cornerPixels;
            }
        ZT_TRUE("выбранная таблица показана уголками", cornerPixels > 0);
    }

    // А КАРЕТКИ ВНУТРИ НЕТ. Спрашиваем правило, а не картинку: под Xvfb
    // hasFocus() лжёт, и по снимку это не проверить.
    ZT_TRUE("каретка не рисуется внутри нарисованной таблицы",
            !zametti::caretShouldBeDrawn(true, false, false, true));
    ZT_TRUE("в обычном тексте каретка есть", zametti::caretShouldBeDrawn(true, false, false, false));

    // Щелчок по НИЖНЕЙ части сетки — тоже выбор таблицы, а не соседний абзац.
    const QPoint low = editor.viewportPoint(QPointF(area.center().x(), area.bottom() - 4));
    QTest::mouseClick(editor.viewport(), Qt::LeftButton, Qt::NoModifier, low);
    QTest::qWait(30);
    ZT_TRUE("щелчок по низу сетки выбрал таблицу",
            zametti::objectOf(editor.textCursor().block()).kind == zametti::ObjectKind::Table);

    // Двойной щелчок по ячейке «3» (последний ряд, первая колонка) — правка
    // исходника, каретка на этой ячейке.
    const zametti::TableRender* render = editor.tableRenderFor(editor.document()->findBlockByNumber(number));
    ZT_TRUE("раскладка есть", render != nullptr);
    if (render == nullptr) return;
    const zametti::TableCellBox* cell = render->layout.at(2, 0);
    ZT_TRUE("ячейка (2,0) есть", cell != nullptr);
    if (cell == nullptr) return;
    const QPointF cellPoint = area.topLeft() + cell->rect.center();
    int hitBlock = -1, hitRow = -1, hitColumn = -1, hitOffset = -1;
    ZT_TRUE("под точкой ячейка", editor.tableCellAt(cellPoint, &hitBlock, &hitRow, &hitColumn, &hitOffset));
    ZT_EQ("это ряд 2", n(2), n(hitRow));
    ZT_EQ("колонка 0", n(0), n(hitColumn));
    QTest::mouseDClick(editor.viewport(), Qt::LeftButton, Qt::NoModifier, editor.viewportPoint(cellPoint));
    QTest::qWait(40);
    const QTextBlock opened = editor.document()->findBlockByNumber(number);
    ZT_TRUE("двойной щелчок раскрыл исходник", isOpenedTable(opened));
    ZT_EQ("каретка на ячейке «3»", n(hitOffset), n(editor.textCursor().positionInBlock()));
    ZT_TRUE("каретка стоит перед «3»",
            editor.textCursor().block().text().mid(editor.textCursor().positionInBlock(), 1) == QStringLiteral("3"));
}

// --- протокол правки: Enter — строка внутри, Esc — свернуть судьёй файла --------

void checkEditProtocol() {
    Editor editor;
    editor.openText(QStringLiteral("протокол"), "до\n\n| a | b |\n|---|---|\n| 1 | 2 |\n| 3 | 4 |\n\nпосле\n");
    QVector<int> tables = tableBlocks(editor);
    ZT_TRUE("таблица есть", tables.size() == 1);
    if (tables.size() != 1) return;
    const int number = tables.first();

    // Входим в правку и встаём в конец первого ряда тела.
    editor.setTextCursor(QTextCursor(editor.document()->findBlockByNumber(number)));
    QTest::keyClick(&editor, Qt::Key_Return);
    QTest::qWait(40);
    QTextBlock opened = editor.document()->findBlockByNumber(number);
    ZT_TRUE("в правку вошли", isOpenedTable(opened));
    const int rowStart = int(opened.text().indexOf(QStringLiteral("| 1 |")));
    QTextCursor at(opened);
    at.setPosition(opened.position() + rowStart + 9);   // конец «| 1 | 2 |»
    editor.setTextCursor(at);
    QTest::qWait(10);

    // ENTER ВНУТРИ ПРАВКИ ВСТАВЛЯЕТ СТРОКУ того же блока и правку не прерывает.
    const int blocksBefore = editor.document()->blockCount();
    QTest::keyClick(&editor, Qt::Key_Return);
    QTest::qWait(40);
    ZT_EQ("Enter не завёл нового блока", n(blocksBefore), n(editor.document()->blockCount()));
    opened = editor.document()->findBlockByNumber(number);
    ZT_TRUE("после Enter таблица всё ещё раскрыта", zametti::isRawBlock(opened) && !zametti::isTableObjectBlock(opened));
    QTest::keyClicks(&editor, QStringLiteral("| 5 | 6 |"));
    QTest::qWait(40);
    ZT_TRUE("набор ряда не прервал правку",
            zametti::isRawBlock(editor.textCursor().block()) && !zametti::isTableObjectBlock(editor.textCursor().block()));

    // Esc сворачивает: снова объект, в нём четыре ряда, файл — с новым рядом.
    QTest::keyClick(&editor, Qt::Key_Escape);
    QTest::qWait(60);
    tables = tableBlocks(editor);
    ZT_TRUE("после Esc таблица снова объект", tables.size() == 1 && tables.first() == number);
    if (tables.size() == 1) {
        const zametti::TableRender* render = editor.tableRenderFor(editor.document()->findBlockByNumber(number));
        ZT_TRUE("в свёрнутой таблице четыре ряда", render != nullptr && render->layout.rows == 4);
    }
    ZT_EQ("файл после правки", std::string("до\n\n| a | b |\n|---|---|\n| 1 | 2 |\n| 5 | 6 |\n| 3 | 4 |\n\nпосле\n"),
          markdown(editor));
    ZT_TRUE("после Esc каретка на таблице",
            zametti::objectOf(editor.textCursor().block()).kind == zametti::ObjectKind::Table);

    // Ctrl+Z возвращает раскрытую таблицу, а не съедает правку.
    QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier);
    QTest::qWait(40);
    ZT_TRUE("Ctrl+Z после Esc возвращает раскрытую таблицу",
            isOpenedTable(editor.document()->findBlockByNumber(number)));
    QTest::keyClick(&editor, Qt::Key_Escape);
    QTest::qWait(40);

    // Пустая строка ВНУТРИ раскрытой таблицы кончает её, как в файле: после
    // Esc судья файла отдаёт таблицу и абзац — ровно то, что прочёл бы файл.
    editor.setTextCursor(QTextCursor(editor.document()->findBlockByNumber(number)));
    QTest::keyClick(&editor, Qt::Key_Return);
    QTest::qWait(40);
    QTextCursor tail(editor.document()->findBlockByNumber(number));
    tail.movePosition(QTextCursor::EndOfBlock);
    editor.setTextCursor(tail);
    QTest::keyClick(&editor, Qt::Key_Return);
    QTest::keyClick(&editor, Qt::Key_Return);
    QTest::keyClicks(&editor, QStringLiteral("tail"));
    QTest::qWait(20);
    QTest::keyClick(&editor, Qt::Key_Escape);
    QTest::qWait(60);
    const std::string afterSplit = markdown(editor);
    ZT_EQ("после Esc — таблица и абзац, как прочёл бы файл",
          std::string("до\n\n| a | b |\n|---|---|\n| 1 | 2 |\n| 5 | 6 |\n| 3 | 4 |\n\ntail\n\nпосле\n"),
          afterSplit);
    ZT_EQ("и то же самое читается из файла", afterSplit, markdownOf(pieces(afterSplit)));
    ZT_TRUE("таблица снова объект", zametti::isTableObjectBlock(editor.document()->findBlockByNumber(number)));

    // Уход каретки наружу тоже сворачивает.
    editor.setTextCursor(QTextCursor(editor.document()->findBlockByNumber(number)));
    QTest::keyClick(&editor, Qt::Key_Return);
    QTest::qWait(40);
    ZT_TRUE("второй вход в правку", isOpenedTable(editor.document()->findBlockByNumber(number)));
    editor.setTextCursor(QTextCursor(editor.document()->firstBlock()));
    QTest::qWait(60);
    ZT_TRUE("уход каретки свернул таблицу", zametti::isTableObjectBlock(editor.document()->findBlockByNumber(number)));
}

// --- switch-and-return == fresh open --------------------------------------------

void checkSwitchAndReturn() {
    Editor editor;
    editor.openText(QStringLiteral("возврат"), kNote, 1000, 700);
    QVector<QRectF> fresh;
    for (const int number : tableBlocks(editor)) fresh.push_back(editor.tableRect(number));
    ZT_TRUE("свежее открытие: три таблицы", fresh.size() == 3);

    const QString other = QDir(g_dir).filePath(QStringLiteral("возврат-другая.md"));
    QFile second(other);
    if (second.open(QIODevice::WriteOnly | QIODevice::Truncate)) second.write("# Другая\n");
    second.close();
    editor.openFile(other);
    QTest::qWait(60);
    editor.openFile(QDir(g_dir).filePath(QStringLiteral("возврат.md")));
    QTest::qWait(120);
    (void)editor.document()->documentLayout()->documentSize();
    QVector<QRectF> back;
    for (const int number : tableBlocks(editor)) back.push_back(editor.tableRect(number));
    ZT_EQ("после возврата таблиц столько же", n(fresh.size()), n(back.size()));
    for (int i = 0; i < qMin(fresh.size(), back.size()); ++i)
        ZT_TRUE("геометрия таблицы " + n(i) + " после возврата та же (" +
                    n(int(back[i].top())) + "×" + n(int(back[i].height())) + " против " +
                    n(int(fresh[i].top())) + "×" + n(int(fresh[i].height())) + ")",
                qAbs(back[i].top() - fresh[i].top()) < 1.5 && qAbs(back[i].height() - fresh[i].height()) < 1.5);
}

// --- живые заметки владельца: файл не меняется, вторая таблица правится ---------

void checkFilesUntouched(const QStringList& sources) {
    for (const QString& source : sources) {
        QFile in(source);
        if (!in.open(QIODevice::ReadOnly)) continue;
        const QByteArray original = in.readAll();
        in.close();
        const QString copy = QDir(g_dir).filePath(QFileInfo(source).fileName());
        QFile out(copy);
        if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) continue;
        out.write(original);
        out.close();

        // Открытие приводит файл к канону (это правило хранилища, а не показ):
        // сперва даём ему причесаться, и только потом меряем — показ и правка
        // кареткой не имеют права тронуть КАНОНИЧЕСКИЙ файл ни на байт.
        {
            zametti::NoteEditor first;
            first.resize(900, 700);
            first.show();
            QTest::qWait(20);
            first.openFile(copy);
            QTest::qWait(80);
        }
        QFile canon(copy);
        if (!canon.open(QIODevice::ReadOnly)) continue;
        const QByteArray before = canon.readAll();
        canon.close();

        zametti::NoteEditor editor;
        editor.resize(900, 700);
        editor.show();
        QTest::qWait(20);
        editor.openFile(copy);
        QTest::qWait(80);
        for (int i = 0; i < 40; ++i) QTest::keyClick(&editor, Qt::Key_Down);
        for (int i = 0; i < 10; ++i) QTest::keyClick(&editor, Qt::Key_Up);
        QTest::qWait(50);
        QTextCursor top = editor.textCursor();
        top.setPosition(0);
        editor.setTextCursor(top);
        QTest::qWait(60);
        editor.grab().toImage().save(QDir(g_dir).filePath(QFileInfo(source).completeBaseName() + QStringLiteral(".png")));

        QFile after(copy);
        ZT_TRUE("копия читается: " + copy.toStdString(), after.open(QIODevice::ReadOnly));
        const QByteArray now = after.readAll();
        after.close();
        ZT_TRUE("файл не изменился при показе — " + QFileInfo(source).fileName().toStdString(), now == before);
    }
}

void checkSecondTableInLiveNote(const QString& source) {
    QFile in(source);
    if (!in.open(QIODevice::ReadOnly)) return;
    const QByteArray body = in.readAll();
    in.close();
    const QString copy = QDir(g_dir).filePath(QStringLiteral("вторая-таблица.md"));
    QFile out(copy);
    if (out.open(QIODevice::WriteOnly | QIODevice::Truncate)) out.write(body);
    out.close();

    Editor editor;
    editor.resize(1100, 800);
    editor.show();
    QTest::qWait(30);
    editor.openFile(copy);
    QTest::qWait(150);
    (void)editor.document()->documentLayout()->documentSize();
    const QVector<int> tables = tableBlocks(editor);
    ZT_TRUE("в живой заметке хотя бы две таблицы (" + n(tables.size()) + ")", tables.size() >= 2);
    if (tables.size() < 2) return;
    const int second = tables[1];
    const QRectF area = editor.tableRect(second);
    editor.verticalScrollBar()->setValue(qMax(0, int(area.top()) - 100));
    QTest::qWait(50);
    QTest::mouseClick(editor.viewport(), Qt::LeftButton, Qt::NoModifier, editor.viewportPoint(area.center()));
    QTest::qWait(50);
    ZT_TRUE("щелчок выбрал вторую таблицу", editor.textCursor().blockNumber() == second);
    QTest::keyClick(&editor, Qt::Key_Return);
    QTest::qWait(100);
    const QTextBlock opened = editor.document()->findBlockByNumber(second);
    ZT_TRUE("Enter по второй таблице раскрыл исходник", isOpenedTable(opened));
    ZT_TRUE("раскрытая таблица имеет высоту",
            editor.document()->documentLayout()->blockBoundingRect(opened).height() > 0.5);
    editor.setTextCursor(QTextCursor(editor.document()->firstBlock()));
    QTest::qWait(60);
    ZT_TRUE("уход каретки свернул вторую таблицу", zametti::isTableObjectBlock(editor.document()->findBlockByNumber(second)));
}

// --- таблица на чужом фоне ------------------------------------------------------
//
// В окне About тот же вьюер показывает справку, палитру ему не правят, и под
// каждой таблицей вылезало молочное пятно «цвета страницы» из облика (заметил
// владелец). У объекта под сеткой закрашивать нечего — но проверка остаётся:
// цвета страницы из облика на месте таблицы быть не должно ни точки.
void checkTableOnForeignBackground() {
    zametti::NoteView view;
    view.setReadOnly(true);
    const QColor paper(0x30, 0x60, 0x90);
    QPalette colours = view.palette();
    colours.setColor(QPalette::Base, paper);
    view.setPalette(colours);
    view.resize(800, 500);
    view.show();
    QTest::qWait(20);

    auto* document = new QTextDocument(&view);
    zametti::buildDocument(pieces("| a | b |\n|---|---|\n| 1 | 2 |\n"), *document);
    view.setDocument(document);
    view.applyContentWidth();
    QTest::qWait(60);
    const QVector<int> tables = tableBlocks(view);
    ZT_TRUE("таблица на чужом фоне показана объектом", tables.size() == 1);
    if (tables.size() != 1) return;
    const QRectF area = view.tableRect(tables.first());
    const QImage shot = view.grab().toImage();
    const QColor page = zametti::settings().style().pageBackground();
    const QPoint origin = view.viewport()->mapTo(&view, QPoint(0, 0));
    int stale = 0;
    for (int px = int(area.left()); px < int(area.right()); ++px)
        for (int py = int(area.top()); py < int(area.bottom()); ++py) {
            const int sx = px + origin.x();
            const int sy = py + origin.y();
            if (sx < 0 || sy < 0 || sx >= shot.width() || sy >= shot.height()) continue;
            if (shot.pixelColor(sx, sy) == page) ++stale;
        }
    ZT_EQ("под таблицей нет точек цвета страницы из облика", n(0), n(stale));
}

}  // namespace

static int ztRunSuite(int argc, char** argv) {
    g_dir = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QDir::tempPath();
    QDir().mkpath(g_dir);
    shoot(QStringLiteral("таблица-широкое"), 1000, 700, kNote);
    shoot(QStringLiteral("таблица-узкое"), 620, 700, kNote);
    checkFlip();
    checkMouse();
    checkEditProtocol();
    checkSwitchAndReturn();
    checkTableOnForeignBackground();

    QStringList sources;
    for (int i = 2; i < argc; ++i) sources << QString::fromLocal8Bit(argv[i]);
    if (!sources.isEmpty()) {
        checkFilesUntouched(sources);
        checkSecondTableInLiveNote(sources.first());
    } else {
        std::printf("копий заметок владельца с таблицами нет — живые проверки пропущены\n");
    }

    std::printf("снимки: %s\n", qPrintable(g_dir));
    return zt::report("снимки таблиц");
}

TEST(TableShots, All) {
    std::vector<QByteArray> ztArgs{QByteArrayLiteral("table_shots_test")};
    ztArgs.push_back((zt::TestData::outDir(QStringLiteral("table-shots"))).toLocal8Bit());
    // Заметки владельца с таблицами — из копии хранилища (.testdata/owner-copy),
    // оригинал не трогается. Первой идёт та, где таблиц хотя бы две.
    const QString corpus = zt::TestData::corpus(QStringLiteral("owner-copy"));
    if (!corpus.isEmpty()) {
        QStringList withTables;
        QString twoTables;
        QDirIterator it(corpus, {QStringLiteral("*.md")}, QDir::Files);
        while (it.hasNext()) {
            const QString path = it.next();
            QFile f(path);
            if (!f.open(QIODevice::ReadOnly)) continue;
            // Таблицы считает тот же разбор, что и заметка: «|---» в блоке кода
            // таблицей не является.
            int tables = 0;
            for (const zametti::Piece& piece : pieces(f.readAll().toStdString()))
                if (piece.raw && piece.table) ++tables;
            if (tables == 0) continue;
            if (tables >= 2 && twoTables.isEmpty()) twoTables = path;
            else if (withTables.size() < 4) withTables << path;
        }
        if (!twoTables.isEmpty()) withTables.prepend(twoTables);
        for (const QString& path : withTables) ztArgs.push_back(path.toLocal8Bit());
    }
    std::vector<char*> ztArgv;
    for (QByteArray& a : ztArgs) ztArgv.push_back(a.data());
    EXPECT_EQ(0, ztRunSuite(int(ztArgv.size()), ztArgv.data()));
}
