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

}  // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    g_dir = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QDir::tempPath();
    QDir().mkpath(g_dir);
    zametti::loadEmbeddedFonts();
    QString error;
    if (!zametti::Formulas::init(&error))
        std::printf("движок формул не поднялся (%s)\n", qPrintable(error));

    // Оба размера окна: целый класс расхождений виден только в узком.
    checkGeometry(1000, QStringLiteral("широкое"));
    checkGeometry(620, QStringLiteral("узкое"));

    return zt::report("геометрия блоков");
}
