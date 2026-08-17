// Сколько места занимает каждый блок. Один документ, все виды сразу.
//
// Набор появился по прямому требованию владельца, и требование верное: «есть ли
// у нас регрессионный тест с объектами разных видов и текстом в одном документе
// и проверка, сколько каждый блок занимает места по вертикали и горизонтали?
// Почему всё ломается постоянно?»
//
// Ломалось потому, что места под объект просили ТРИ разных куска кода, каждый
// по-своему, и проверялись они порознь — снимками, на которых видно «красиво
// или нет», а не «сколько». Здесь спрашивается ровно «сколько», и сразу про
// все виды:
//
//   * блок не выше своего содержимого плюс отбивка абзаца — это ловит полосы
//     пустоты (владелец видел их между формулами, потом между таблицами);
//   * содержимое не вылезает за прямоугольник своего блока — это ловит мусор
//     при прокрутке: перерисовку Qt заказывает по границе блока;
//   * ширина содержимого не выходит за колонку текста;
//   * соседние объекты не наползают друг на друга.
//
// Мера отбивки — строка текста: больше строки пустоты человек читает как дыру.

#include "block_object.h"
#include "doc_model.h"
#include "editor_widget.h"
#include "formula.h"
#include "note_view.h"
#include "resources.h"

#include "test_util.h"
#include "testdata.h"

#include <QAbstractTextDocumentLayout>
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFontMetricsF>
#include <QTest>
#include <QTextBlock>
#include <QImage>
#include <QScrollBar>
#include <QTextDocument>

#include <string>
#include <vector>

namespace {

QString g_dir;

// Заметка со всеми видами блоков и объектов. Две таблицы и две формулы подряд —
// именно на соседстве однотипных объектов и вылезали полосы пустоты.
const char* kNote = R"(# Геометрия

Обычный абзац перед объектами.

$$\frac{a}{b}$$

$$\sum_{k=0}^\infty \frac{x^k}{k!}$$

| a | b |
|---|---|
| 1 | 2 |

| c | d |
|---|---|
| 3 | 4 |

```python
код = 1
```

- пункт списка
- второй пункт

> цитата

___

Хвост заметки со [ссылкой](https://example.com) — её видно всегда.

$$\lim_{h\to0} \frac{f(x+h)-f(x)}{h}$$

Последние две строки после формулы: ссылка [вторая](https://example.org)
и обычный текст. Именно они пропадали после возврата на заметку.
)";

struct Box {
    int block = -1;
    QString what;
    qreal blockHeight = 0;   // сколько занял блок вместе со своим полем
    qreal contentHeight = 0; // сколько занимает то, что в нём показано
    QRectF content;          // прямоугольник содержимого в координатах документа
    QRectF area;             // прямоугольник блока
};

// Что и сколько занимает. Пусто — обычный текст, его меряет сам Qt.
const QAbstractTextDocumentLayout* layoutOf(zametti::NoteEditor& editor) {
    return editor.document()->documentLayout();
}

std::vector<Box> boxesOf(zametti::NoteEditor& editor) {
    std::vector<Box> out;
    const QAbstractTextDocumentLayout* layout = editor.document()->documentLayout();
    for (QTextBlock b = editor.document()->firstBlock(); b.isValid(); b = b.next()) {
        Box box;
        box.block = b.blockNumber();
        box.area = layout->blockBoundingRect(b);
        box.blockHeight = box.area.height() + b.blockFormat().bottomMargin();

        if (const zametti::FormulaRender* formula = editor.formulaAt(b.blockNumber())) {
            box.what = QStringLiteral("формула");
            box.content = editor.formulaRect(b.blockNumber());
            box.contentHeight = formula->height;
        } else if (const zametti::TableRender* table = editor.tableAt(b.blockNumber())) {
            box.what = QStringLiteral("таблица");
            box.content = editor.tableRect(b.blockNumber());
            box.contentHeight = table->layout.height;
            // МЕРЯЕМ ПО ПОСЛЕДНЕЙ СТРОКЕ. Строки таблицы, кроме последней,
            // спрятаны, а спрятанный блок Qt отдаёт нулевым прямоугольником в
            // начале координат — считать от него значит мерить пустоту (моя
            // первая редакция получала 406 точек там, где их сотня).
            const QTextBlock last = editor.document()->findBlockByNumber(table->last);
            if (!last.isValid()) continue;
            box.area = layout->blockBoundingRect(last);
            box.blockHeight = box.area.height() + last.blockFormat().bottomMargin();
        } else {
            continue;
        }
        out.push_back(box);
    }
    return out;
}

void checkGeometry(int width, const QString& name) {
    const QString path = QDir(g_dir).filePath(name + QStringLiteral(".md"));
    QFile file(path);
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) file.write(kNote);
    file.close();

    zametti::NoteEditor editor;
    editor.resize(width, 800);
    editor.show();
    QTest::qWait(20);
    editor.openFile(path);
    QTest::qWait(120);

    // УХОД НА ДРУГУЮ ЗАМЕТКУ И ВОЗВРАТ. Заметка откладывается целиком (кэш), и
    // всё, что вид успел записать в живой документ, возвращается вместе с ней.
    // Владелец увидел это так: «сразу после загрузки последние две строки
    // видны, после переключения и возврата — уже нет».
    const QString other = QDir(g_dir).filePath(name + QStringLiteral("-другая.md"));
    QFile second(other);
    if (second.open(QIODevice::WriteOnly | QIODevice::Truncate)) second.write("# Другая\n");
    second.close();
    editor.openFile(other);
    QTest::qWait(60);
    editor.openFile(path);
    QTest::qWait(120);

    const qreal line = QFontMetricsF(editor.baseFont()).height();
    const qreal column = editor.viewport()->width();
    const std::vector<Box> boxes = boxesOf(editor);

    ZT_TRUE(name.toStdString() + ": объектов найдено " + std::to_string(boxes.size()),
            boxes.size() >= 4);

    for (const Box& box : boxes) {
        const std::string who = name.toStdString() + ", " + box.what.toStdString() + " в блоке " +
                                std::to_string(box.block);

        // 1. НЕ РАЗДУТ. Блок занимает своё содержимое плюс отбивку — не больше.
        //
        // Мера одна на все виды — две строки текста: отбивка абзаца плюс
        // запас на округление. Именно эта проверка нашла беду, из-за которой
        // владелец сказал «этим пользоваться просто нельзя»: резерв под сетку
        // применялся ДВАЖДЫ (я оставил два одинаковых куска, переставляя код), и
        // каждая таблица получала двойное поле — на его заметке это выросло в
        // тысячу точек пустоты между двумя табличками.
        const qreal allowance = 2.0 * line;
        ZT_TRUE(who + ": не раздут (" + std::to_string(int(box.blockHeight)) + " при содержимом " +
                    std::to_string(int(box.contentHeight)) + ", строка " +
                    std::to_string(int(line)) + ")",
                box.blockHeight <= box.contentHeight + allowance + 2.0);

        // 2. НЕ ВЫЛЕЗАЕТ. Содержимое внутри своего блока: перерисовку при
        //    прокрутке Qt заказывает по границе блока, и всё, что нарисовано за
        //    ней, остаётся мусором на экране.
        ZT_TRUE(who + ": содержимое не ниже своего блока (низ " +
                    std::to_string(int(box.content.bottom())) + ", блок до " +
                    std::to_string(int(box.area.top() + box.blockHeight)) + ")",
                box.content.isEmpty() || box.content.bottom() <= box.area.top() + box.blockHeight + 1.0);

        // 3. НЕ ШИРЕ КОЛОНКИ.
        ZT_TRUE(who + ": не шире колонки (" + std::to_string(int(box.content.width())) + " при " +
                    std::to_string(int(column)) + ")",
                box.content.isEmpty() || box.content.width() <= column + 1.0);
    }

    // 4. ТЕКСТ ПОСЛЕ ОБЪЕКТА ВИДЕН. Проверка появилась после того, как я
    //    погасил исходник формулы прозрачным цветом, выделив блок ВМЕСТЕ с
    //    разделителем: прозрачность перетекла на следующий блок, и под
    //    последней формулой пропали ссылка и абзац текста.
    {
        const QImage shot = editor.grab().toImage();
        const qreal dpr = editor.devicePixelRatioF();
        const int scroll = editor.verticalScrollBar()->value();
        int seen = 0;
        for (QTextBlock b = editor.document()->firstBlock(); b.isValid(); b = b.next()) {
            if (b.text().trimmed().isEmpty()) continue;
            if (zametti::objectOf(b).valid()) continue;
            if (editor.formulaAt(b.blockNumber()) != nullptr) continue;
            const QRectF area = layoutOf(editor)->blockBoundingRect(b);
            if (area.bottom() - scroll > editor.viewport()->height()) break;
            int dark = 0;
            for (int x = 0; x < shot.width(); ++x)
                for (int y = int((area.top() - scroll) * dpr);
                     y < int((area.bottom() - scroll) * dpr) && y < shot.height(); ++y)
                    if (y >= 0 && qGray(shot.pixel(x, y)) < 160) ++dark;
            ZT_TRUE(name.toStdString() + ": текст блока " + std::to_string(b.blockNumber()) +
                        " виден («" + b.text().left(20).toStdString() + "»)",
                    dark > 5);
            ++seen;
        }
        ZT_TRUE(name.toStdString() + ": текстовых блоков проверено " + std::to_string(seen),
                seen >= 4);
    }

    // 5. НЕ НАПОЛЗАЮТ ДРУГ НА ДРУГА.
    for (size_t i = 1; i < boxes.size(); ++i)
        ZT_TRUE(name.toStdString() + ": объекты " + std::to_string(i) + " и " +
                    std::to_string(i + 1) + " не наползают",
                boxes[i].content.isEmpty() || boxes[i - 1].content.isEmpty() ||
                    boxes[i].content.top() >= boxes[i - 1].content.bottom() - 1.0);
}

// ПОЛОСА ФОРМУЛЫ ПЕРЕЖИВАЕТ СДВИГ БЛОКОВ.
//
// Владелец: «в какой-то момент формулы начинают наползать на текст ниже, нижняя
// часть формул пропадает» — и нарочно не воспроизводится. Подозреваемый: кэш
// вёрстки ключуется НОМЕРОМ БЛОКА и перестраивается по textChanged, а вёрстка
// Qt перемеряет объекты раньше — внутри contentsChange. Стоит вставить абзац
// ВЫШЕ формул, номера съезжают, и полоса формулы меряется по чужому (или по
// пустому) — вёрстка вылезает за свой блок на текст под ней.
//
// Здесь мерится ровно это: после каждой правки выше формул полоса КАЖДОЙ
// формулы не ниже её вёрстки, а сама вёрстка — внутри прямоугольника блока.
// Правка идёт клавишами редактора, как у человека, а не глаголом заметки:
// порядок сигналов и есть предмет проверки.
struct FormulaFit {
    int block = -1;
    qreal band = 0;      // высота блока по вёрстке Qt
    qreal render = 0;    // высота вёрстки формулы
    QRectF area;
    QRectF drawn;        // где вёрстка нарисована
};

std::vector<FormulaFit> formulaFits(zametti::NoteEditor& editor) {
    std::vector<FormulaFit> out;
    const QAbstractTextDocumentLayout* layout = editor.document()->documentLayout();
    for (QTextBlock b = editor.document()->firstBlock(); b.isValid(); b = b.next()) {
        const zametti::FormulaRender* render = editor.formulaAt(b.blockNumber());
        if (render == nullptr) continue;
        FormulaFit fit;
        fit.block = b.blockNumber();
        fit.area = layout->blockBoundingRect(b);
        if (fit.area.height() <= 0.0 && qgetenv("ZT_DEBUG_FITS") == "1")
            std::printf("блок %d: lines=%d bounding=%gx%g pos=%g,%g visible=%d level=%d text=«%s»\n",
                        b.blockNumber(), b.layout() ? b.layout()->lineCount() : -1,
                        b.layout() ? b.layout()->boundingRect().width() : -1.0,
                        b.layout() ? b.layout()->boundingRect().height() : -1.0,
                        b.layout() ? b.layout()->position().x() : -1.0,
                        b.layout() ? b.layout()->position().y() : -1.0,
                        int(b.isVisible()), zametti::levelOf(b),
                        qPrintable(zametti::blockFormulaRef(b).source.left(30)));
        if (fit.area.height() <= 0.0 && qgetenv("ZT_DEBUG_FITS") == "2") {
            editor.document()->markContentsDirty(b.position(), b.length());
            const QRectF again = layout->blockBoundingRect(b);
            const QTextLine line = b.layout()->lineCount() > 0 ? b.layout()->lineAt(0) : QTextLine();
            std::printf("   блок %d после markContentsDirty: rect %gx%g, line height %g natW %g\n",
                        b.blockNumber(), again.width(), again.height(),
                        line.isValid() ? line.height() : -2.0, line.isValid() ? line.naturalTextWidth() : -2.0);
        }
        if (qgetenv("ZT_DEBUG_FITS") == "1" && b.layout() && b.layout()->lineCount() > 0) {
            const QTextLine line = b.layout()->lineAt(0);
            std::printf("   блок %d line: y=%g height=%g ascent=%g descent=%g natW=%g rect=%gx%g lh=%g lhType=%d\n",
                        b.blockNumber(), line.y(), line.height(), line.ascent(), line.descent(), line.naturalTextWidth(),
                        line.rect().width(), line.rect().height(),
                        b.blockFormat().lineHeight(), b.blockFormat().lineHeightType());
        }
        fit.band = fit.area.height();
        fit.render = render->height;
        fit.drawn = editor.formulaRect(b.blockNumber());
        out.push_back(fit);
    }
    return out;
}

void checkFits(zametti::NoteEditor& editor, const std::string& when, size_t atLeast = 3) {
    // Вёрстка ленивая: хвост заметки за окном ещё не размечен, и его блоки
    // отдаются нулевыми. Спрашиваем размер документа — это и доводит вёрстку.
    (void)editor.document()->documentLayout()->documentSize();
    const std::vector<FormulaFit> fits = formulaFits(editor);
    ZT_TRUE(when + ": формул найдено " + std::to_string(fits.size()), fits.size() >= atLeast);
    for (const FormulaFit& fit : fits) {
        const std::string who = when + ", формула в блоке " + std::to_string(fit.block);
        ZT_TRUE(who + ": полоса не ниже вёрстки (полоса " + std::to_string(int(fit.band)) +
                    ", вёрстка " + std::to_string(int(fit.render)) + ")",
                fit.band + 1.0 >= fit.render);
        ZT_TRUE(who + ": вёрстка внутри блока (низ " + std::to_string(int(fit.drawn.bottom())) +
                    ", блок до " + std::to_string(int(fit.area.bottom())) + ")",
                fit.drawn.isEmpty() || fit.drawn.bottom() <= fit.area.bottom() + 1.0);
    }
}

void checkFormulaBandSurvivesShift(int width, const QString& name) {
    const QString path = QDir(g_dir).filePath(name + QStringLiteral("-сдвиг.md"));
    QFile file(path);
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) file.write(kNote);
    file.close();

    zametti::NoteEditor editor;
    editor.resize(width, 800);
    editor.show();
    QTest::qWait(20);
    editor.openFile(path);
    QTest::qWait(120);
    const std::string tag = name.toStdString();
    checkFits(editor, tag + ": сразу после открытия");

    // Абзац НАД формулами: Enter в начале заголовка отбивает его сверху пустой
    // строкой — номера всех блоков ниже съезжают на один.
    QTextCursor at = editor.textCursor();
    at.setPosition(0);
    editor.setTextCursor(at);
    QTest::keyClick(&editor, Qt::Key_Return);
    QTest::qWait(60);
    checkFits(editor, tag + ": после Enter над формулами");

    // Ещё два: между формулами теперь чужие номера в старом кэше.
    QTest::keyClick(&editor, Qt::Key_Return);
    QTest::qWait(30);
    QTest::keyClick(&editor, Qt::Key_Return);
    QTest::qWait(60);
    checkFits(editor, tag + ": после трёх Enter над формулами");

    // И обратно: Backspace убирает строку, номера едут вверх.
    QTest::keyClick(&editor, Qt::Key_Backspace);
    QTest::qWait(60);
    checkFits(editor, tag + ": после Backspace над формулами");

    // Набор в абзаце ПЕРЕД формулами: правка без сдвига номеров, но с
    // перевёрсткой соседей.
    QTextCursor para = editor.textCursor();
    para.movePosition(QTextCursor::NextBlock);
    para.movePosition(QTextCursor::NextBlock);
    para.movePosition(QTextCursor::EndOfBlock);
    editor.setTextCursor(para);
    QTest::keyClicks(&editor, QStringLiteral(" more words"));
    QTest::qWait(60);
    checkFits(editor, tag + ": после набора над формулами");

    // Раскрыть первую формулу, дописать, свернуть: блок пересобран на месте.
    {
        std::vector<FormulaFit> fits = formulaFits(editor);
        ZT_TRUE(tag + ": есть формула для правки", !fits.empty());
        if (!fits.empty()) {
            editor.setTextCursor(QTextCursor(editor.document()->findBlockByNumber(fits[0].block)));
            QTest::keyClick(&editor, Qt::Key_Return);   // раскрыть
            QTest::qWait(40);
            // Каретка встаёт в конец исходника — за закрывающими долларами;
            // дописываем ВНУТРЬ формулы, иначе она законно перестанет ею быть.
            QTest::keyClick(&editor, Qt::Key_Left);
            QTest::keyClick(&editor, Qt::Key_Left);
            QTest::keyClicks(&editor, QStringLiteral("+1"));
            QTest::qWait(40);
            QTest::keyClick(&editor, Qt::Key_Escape);   // свернуть
            QTest::qWait(60);
            checkFits(editor, tag + ": после правки формулы");
            // Отмена правки и возврат — блок снова пересобран.
            QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier);
            QTest::qWait(40);
            QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier);
            QTest::qWait(60);
            // Второй Ctrl+Z вернул формулу раскрытой: объектов на один меньше.
            checkFits(editor, tag + ": после двух Ctrl+Z", 2);
            QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
            QTest::qWait(60);
            checkFits(editor, tag + ": после Ctrl+Shift+Z", 2);
            QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
            QTest::qWait(60);
            checkFits(editor, tag + ": после второго Ctrl+Shift+Z (формула снова свёрнута)");
        }
    }

    // Набор в абзаце СРАЗУ ПОД формулой и удаление его строки.
    {
        std::vector<FormulaFit> fits = formulaFits(editor);
        if (fits.size() >= 2) {
            QTextBlock below = editor.document()->findBlockByNumber(fits[1].block).next();
            while (below.isValid() && below.text().trimmed().isEmpty()) below = below.next();
            if (below.isValid()) {
                editor.setTextCursor(QTextCursor(below));
                QTest::keyClicks(&editor, QStringLiteral("xyz "));
                QTest::qWait(60);
                checkFits(editor, tag + ": после набора под формулой");
                for (int i = 0; i < 2; ++i) QTest::keyClick(&editor, Qt::Key_Backspace);
                QTest::qWait(60);
                checkFits(editor, tag + ": после стирания под формулой");
            }
        }
    }

    // СЛУЧАЙ ВЛАДЕЛЬЦА: убрать формулу (Backspace на ней) и вернуть Ctrl+Z —
    // «матрица опускается ниже Delimiters», хотя после Ctrl+Z, по нашей
    // аксиоме, править ничего не надо.
    {
        std::vector<FormulaFit> fits = formulaFits(editor);
        if (fits.size() >= 2) {
            editor.setTextCursor(QTextCursor(editor.document()->findBlockByNumber(fits[1].block)));
            QTest::keyClick(&editor, Qt::Key_Backspace);   // объект — атом: убран целиком
            QTest::qWait(60);
            ZT_TRUE(tag + ": формула убрана Backspace", formulaFits(editor).size() == fits.size() - 1);
            QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier);
            QTest::qWait(60);
            ZT_TRUE(tag + ": формула вернулась Ctrl+Z", formulaFits(editor).size() == fits.size());
            checkFits(editor, tag + ": после удаления формулы и Ctrl+Z");
        }
    }

    // Зум туда и обратно.
    editor.setZoom(1.5);
    QTest::qWait(80);
    checkFits(editor, tag + ": после зума 150%");
    editor.setZoom(1.0);
    QTest::qWait(80);
    checkFits(editor, tag + ": после зума 100%");

    // Уход и возврат: заметка вернулась из кэша.
    const QString other = QDir(g_dir).filePath(name + QStringLiteral("-сдвиг-другая.md"));
    QFile second(other);
    if (second.open(QIODevice::WriteOnly | QIODevice::Truncate)) second.write("# Другая\n\nтекст\n");
    second.close();
    editor.openFile(other);
    QTest::qWait(60);
    editor.openFile(path);
    QTest::qWait(120);
    checkFits(editor, tag + ": после ухода-возврата");
    QTextCursor top = editor.textCursor();
    top.setPosition(0);
    editor.setTextCursor(top);
    QTest::keyClick(&editor, Qt::Key_Return);
    QTest::qWait(60);
    checkFits(editor, tag + ": после возврата и Enter над формулами");
}

// ТОТ ЖЕ СЛУЧАЙ НА КОПИИ ЗАМЕТКИ ВЛАДЕЛЬЦА («Typesetting Math in Markdown»):
// правило проекта — названный случай проверяется сам, а не его дистиллят.
// Рецепт владельца: под матрицей, над «## Delimiters», Backspace убирает
// формулу; Ctrl+Z возвращает — и матрица опускается на Delimiters.
void checkOwnerRecipe(int width, const QString& name) {
    const QString source = zt::TestData::file(QStringLiteral("formula-shift/typesetting-math.md"));
    if (source.isEmpty()) {
        std::printf("копии заметки владельца (formula-shift/typesetting-math.md) нет — рецепт пропущен\n");
        return;
    }
    const QString path = QDir(g_dir).filePath(name + QStringLiteral("-typesetting.md"));
    QFile::remove(path);
    QFile::copy(source, path);

    zametti::NoteEditor editor;
    editor.resize(width, 800);
    editor.show();
    QTest::qWait(20);
    editor.openFile(path);
    QTest::qWait(200);
    const std::string tag = name.toStdString() + ", заметка владельца";
    checkFits(editor, tag + ": после открытия");

    // Матрица — формула прямо над «## Delimiters».
    QTextBlock heading;
    for (QTextBlock b = editor.document()->firstBlock(); b.isValid(); b = b.next())
        if (b.text().startsWith(QStringLiteral("Delimiters"))) { heading = b; break; }
    ZT_TRUE(tag + ": заголовок Delimiters найден", heading.isValid());
    if (!heading.isValid()) return;
    QTextBlock matrix = heading.previous();
    while (matrix.isValid() && editor.formulaAt(matrix.blockNumber()) == nullptr) matrix = matrix.previous();
    ZT_TRUE(tag + ": матрица над Delimiters найдена", matrix.isValid());
    if (!matrix.isValid()) return;
    const int formulas = int(formulaFits(editor).size());

    editor.setTextCursor(QTextCursor(matrix));
    QTest::keyClick(&editor, Qt::Key_Backspace);
    QTest::qWait(80);
    ZT_TRUE(tag + ": матрица убрана", int(formulaFits(editor).size()) == formulas - 1);
    QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier);
    QTest::qWait(120);
    ZT_TRUE(tag + ": матрица вернулась", int(formulaFits(editor).size()) == formulas);
    checkFits(editor, tag + ": после Backspace и Ctrl+Z");

    // И буквально: Delimiters ниже низа матрицы.
    QTextBlock again;
    for (QTextBlock b = editor.document()->firstBlock(); b.isValid(); b = b.next())
        if (b.text().startsWith(QStringLiteral("Delimiters"))) { again = b; break; }
    QTextBlock back = again.isValid() ? again.previous() : QTextBlock();
    while (back.isValid() && editor.formulaAt(back.blockNumber()) == nullptr) back = back.previous();
    if (again.isValid() && back.isValid()) {
        const QRectF drawn = editor.formulaRect(back.blockNumber());
        const QRectF head = layoutOf(editor)->blockBoundingRect(again);
        ZT_TRUE(tag + ": Delimiters ниже матрицы (заголовок с " + std::to_string(int(head.top())) +
                    ", матрица до " + std::to_string(int(drawn.bottom())) + ")",
                head.top() >= drawn.bottom() - 1.0);
    }
}

}  // namespace

static int ztRunSuite(int argc, char** argv) {
    g_dir = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QDir::tempPath();
    QDir().mkpath(g_dir);
    zametti::loadEmbeddedFonts();
    QString error;
    if (!zametti::Formulas::init(&error))
        std::printf("движок формул не поднялся (%s)\n", qPrintable(error));

    // Полоса формулы после сдвига блоков — идёт ВСЕГДА: формула объект и
    // показана вёрсткой независимо от того, вернулись ли таблицы.
    checkFormulaBandSurvivesShift(1000, QStringLiteral("широкое"));
    checkFormulaBandSurvivesShift(620, QStringLiteral("узкое"));
    checkOwnerRecipe(1000, QStringLiteral("широкое"));
    checkOwnerRecipe(620, QStringLiteral("узкое"));

    // Оба размера окна: целый класс расхождений виден только в узком.
    if (zametti::kObjectsShown) {
        checkGeometry(1000, QStringLiteral("широкое"));
        checkGeometry(620, QStringLiteral("узкое"));
    } else {
        std::printf("геометрия таблиц пропущена: таблицы показаны исходником (kObjectsShown = false)\n");
    }

    return zt::report("геометрия блоков");
}

// Набор целиком одним TEST: тело не тронуто, argv ему собран здесь.
// Дробить на отдельные проверки — отдельная работа, по одному набору.
TEST(BlockGeometry, All) {
    // Набор меряет геометрию ТАБЛИЦ И ФОРМУЛ (см. boxesOf — фотографий он не
    // собирает вовсе), а они всё ещё показаны исходником: место под них держит
    // вид полями блоков. Фотография из-под этой константы уже вышла — она
    // объект, и её геометрию меряет Image.All.
    //
    // Пропуск привязан К ТОЙ ЖЕ КОНСТАНТЕ, которой снят показ, а не списком в
    // голове: вернётся показ — вернётся и набор, сам, без напоминания.
    // Пропуск теперь ВНУТРИ набора и только для таблиц: полоса формулы меряется
    // всегда — она объект.
    std::vector<QByteArray> ztArgs{QByteArrayLiteral("block_geometry_test")};
    ztArgs.push_back((zt::TestData::outDir(QStringLiteral("block-geometry"))).toLocal8Bit());
    std::vector<char*> ztArgv;
    for (QByteArray& a : ztArgs) ztArgv.push_back(a.data());
    EXPECT_EQ(0, ztRunSuite(int(ztArgv.size()), ztArgv.data()));
}
