// Блоки кода: плашка, полоска с языком, кнопка копирования.
//
// Набор делает две вещи разом. Числом проверяется то, что числом берётся:
// геометрия плашки (полоска стоит в поле блока и на текст не налезает,
// отступы слева ровно те, что заказаны), непричастность полоски к тексту
// (каретка, выделение и поиск её не видят) и содержимое буфера обмена.
// Скругление углов числом не возьмёшь — его спрашиваем У СНИМКА, по пикселю
// в углу плашки.
//
// Остальное — резкость, читаемость, соразмерность — снимок и есть отчёт.
// Каталог печатается в вывод, чтобы не искать.

#include "doc_model.h"
#include "editor_widget.h"
#include "lang_editor.h"
#include "note_view.h"
#include "settings.h"
#include "test_util.h"

#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QPainter>
#include <QSet>
#include <QTest>
#include <QTextBlock>
#include <QTextCursor>
#include <QScrollBar>
#include <QAbstractTextDocumentLayout>
#include <QTextDocument>

#include <string>

namespace {

QString g_store;
QString g_shots;

std::string num(double v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.1f", v);
    return buf;
}

void check(bool ok, const std::string& what) {
    ++zt::g_checks;
    if (ok) return;
    ++zt::g_failures;
    std::printf("провал: %s\n", what.c_str());
}

QString writeNote(const QString& name, const QString& text) {
    const QString path = QDir(g_store).filePath(name);
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return {};
    f.write(text.toUtf8());
    f.close();
    return path;
}

// Полосы подложки и кнопка лежат под защитой: это часть отрисовки, и наружу
// они не нужны никому, кроме приёмки.
class Peek : public zametti::NoteEditor {
public:
    QVector<zametti::CodeBand> bands() {
        return codeBands(QRectF(0, 0, 10000, 100000));
    }
    QRectF button(const zametti::CodeBand& band) { return copyButtonRect(band); }
    QRectF langRect(const zametti::CodeBand& band) { return languageRect(band); }
    QString codeText(int firstBlock) { return codeTextFrom(firstBlock); }
    // Кусок документа, нарисованный так же, как он уходит на бумагу.
    QImage onPaper(const QRectF& area) {
        QImage sheet(int(area.width()), int(area.height()), QImage::Format_RGB32);
        sheet.fill(Qt::white);
        QPainter painter(&sheet);
        painter.translate(-area.left(), -area.top());
        renderSlice(painter, area, 1.0);
        return sheet;
    }
    void copyBlock(int firstBlock) { copyCodeBlock(firstBlock); }
};

const char* kSample = R"(# Блоки кода

Обычный абзац перед блоком: текст живёт своей жизнью, а код — своей.

```python
def solve(n):
    total = 0
    for i in range(n):
        total += i * i
    return total
```

Между блоками — абзац, чтобы видеть, как код отбивается от текста.

```
echo "а этот блок без языка"
ls -la | grep zametti
```

- пункт списка, а под ним код:

    ```c++
    for (int i = 0; i < n; ++i)
        sum += i;
    ```

- следующий пункт

Строчный `код внутри абзаца` — для сравнения с блочным.

```ficus
fun solve(n: int) = fold(s=0) for i <- 0:n {s + i*i}
```
)";

Peek* open(Peek& editor, int width, int height, const QString& name) {
    const QString note = writeNote(name + QStringLiteral(".md"),
                                   QString::fromUtf8(kSample));
    editor.resize(width, height);
    editor.show();
    QTest::qWait(20);
    editor.openFile(note);
    QTest::qWait(60);
    return &editor;
}

void shoot(zametti::NoteEditor& editor, const QString& name) {
    QTextCursor at = editor.textCursor();
    at.setPosition(0);
    editor.setTextCursor(at);
    QTest::qWait(60);
    const QImage shot = editor.grab().toImage();
    const QString path = QDir(g_shots).filePath(name + QStringLiteral(".png"));
    if (!shot.save(path)) std::printf("  НЕ СОХРАНИЛСЯ снимок %s\n", qPrintable(path));
}

// --- полоска не участвует в тексте ------------------------------------------
//
// Имя языка нарисовано в поле блока, и в самом тексте документа его нет. Это
// не придирка: утёкшая в текст служебная надпись — ровно то, чем кончился
// этап 10 (см. «удалено: 2 строки» в отчёте), и спрашивать это надо набором.
void checkStripIsNotText(Peek& editor) {
    const QString whole = editor.document()->toPlainText();
    check(!whole.contains(QStringLiteral("python")), "языка нет в тексте документа");
    check(!whole.contains(QStringLiteral("ficus")), "и второго языка тоже нет");

    QTextCursor all(editor.document());
    all.select(QTextCursor::Document);
    check(!all.selectedText().contains(QStringLiteral("python")),
          "выделение всего документа языка не захватывает");

    // Поиск спрашиваем тот же, что и в окне, — по документу.
    check(editor.document()->find(QStringLiteral("python")).isNull(),
          "поиск языка в документе не находит");
}

// --- геометрия плашки -------------------------------------------------------
void checkPlateGeometry(Peek& editor) {
    const QVector<zametti::CodeBand> bands = editor.bands();
    check(bands.size() >= 9, "полосы собраны по всем строкам кода (" +
                                 std::to_string(bands.size()) + ")");
    if (bands.isEmpty()) return;

    const zametti::CodePlate plate = zametti::codePlate(editor.zoom());
    check(plate.strip > 0.0, "высота полоски положительна");
    check(plate.padBottom > 0.0 && plate.padBottom < plate.strip,
          "поле снизу есть и меньше полоски: " + num(plate.padBottom) + " < " +
              num(plate.strip));

    const QAbstractTextDocumentLayout* layout = editor.document()->documentLayout();
    int first = 0;
    for (const zametti::CodeBand& band : bands) {
        if (!band.first) continue;
        ++first;
        const QTextBlock block = editor.document()->findBlockByNumber(band.blockNumber);
        const QTextBlock before = block.previous();
        if (!before.isValid()) continue;

        // Полоска стоит В РЕЗЕРВЕ, который сборщик отвёл верхним полем блока.
        // Если бы величины разошлись, она налезла бы на предыдущий абзац —
        // это и спрашиваем.
        const qreal stripTop = band.rect.top() - plate.strip;
        const qreal aboveBottom = layout->blockBoundingRect(before).bottom();
        check(stripTop >= aboveBottom - 0.5,
              "полоска не налезает на блок выше: " + num(stripTop) + " >= " +
                  num(aboveBottom));

        // Левый край плашки — на codeIndent правее абзаца, а код — ещё на
        // codePadLeft правее плашки.
        const QRectF rect = layout->blockBoundingRect(block);
        const qreal codeLeft = rect.left() + block.blockFormat().leftMargin();
        check(std::fabs(band.rect.left() - (codeLeft - plate.padLeft)) < 0.5,
              "код отступает от левого края плашки на padLeft");
        const QTextBlock paragraph = editor.document()->firstBlock();
        const qreal columnLeft = layout->blockBoundingRect(paragraph).left();
        if (zametti::levelOf(block) < 0)
            check(std::fabs(band.rect.left() - (columnLeft + plate.indent)) < 0.5,
                  "плашка отступает от абзаца на codeIndent: " + num(band.rect.left()) +
                      " против " + num(columnLeft + plate.indent));
    }
    check(first == 4, "начал блоков ровно четыре (" + std::to_string(first) + ")");

    // Последняя строка блока несёт нижнее поле, и плашка на нём кончается.
    for (const zametti::CodeBand& band : bands) {
        if (!band.last) continue;
        const QTextBlock block = editor.document()->findBlockByNumber(band.blockNumber);
        check(std::fabs(block.blockFormat().bottomMargin() - plate.padBottom) < 0.5,
              "нижнее поле у последней строки блока");
    }
}

// --- копирование ------------------------------------------------------------
//
// Договор из брифа: в буфер уходит текст кода с переводами строк, без заборов
// и без имени языка.
void checkCopy(Peek& editor) {
    const QVector<zametti::CodeBand> bands = editor.bands();
    int firstBlock = -1;
    for (const zametti::CodeBand& band : bands)
        if (band.first && band.info == QStringLiteral("python")) firstBlock = band.blockNumber;
    check(firstBlock >= 0, "блок на python найден");
    if (firstBlock < 0) return;

    const QString expected =
        QStringLiteral("def solve(n):\n    total = 0\n    for i in range(n):\n"
                       "        total += i * i\n    return total");
    check(editor.codeText(firstBlock) == expected, "текст блока — весь код и только код");

    QGuiApplication::clipboard()->setText(QStringLiteral("прежнее"));
    editor.copyBlock(firstBlock);
    const QString got = QGuiApplication::clipboard()->text();
    check(got == expected, "в буфере ровно код");
    check(!got.contains(QStringLiteral("```")), "заборов в буфере нет");
    check(!got.contains(QStringLiteral("python")), "языка в буфере нет");

    // Кнопка нарисована в полоске, а не в тексте: её прямоугольник лежит выше
    // первой строки кода.
    for (const zametti::CodeBand& band : bands) {
        if (band.blockNumber != firstBlock) continue;
        const QRectF box = editor.button(band);
        check(!box.isEmpty(), "кнопка копирования есть");
        check(box.bottom() <= band.rect.top() + 0.5,
              "кнопка стоит над первой строкой кода");
        check(box.right() <= band.rect.right() + 0.5, "кнопка не вылезает за плашку");

        // НАСТОЯЩЕЕ НАЖАТИЕ, а не вызов метода: между «функция кладёт в буфер»
        // и «по кнопке кладётся в буфер» лежит вся обработка мыши, и именно
        // там у нас в прошлые этапы всё и ломалось. Заодно спрашиваем, что
        // каретка от нажатия не уехала: кнопка нарисована в поле блока, и без
        // перехвата щелчок ставил бы туда курсор.
        QGuiApplication::clipboard()->setText(QStringLiteral("прежнее"));
        QTextCursor before = editor.textCursor();
        before.setPosition(0);
        editor.setTextCursor(before);
        const QPoint at(int(box.center().x()) - editor.horizontalScrollBar()->value(),
                        int(box.center().y()) - editor.verticalScrollBar()->value());
        QTest::mouseClick(editor.viewport(), Qt::LeftButton, Qt::NoModifier, at);
        QTest::qWait(20);
        check(QGuiApplication::clipboard()->text() == expected,
              "нажатие на кнопку кладёт код в буфер");
        check(editor.textCursor().position() == 0, "каретка от нажатия не уехала");
    }
}

// --- скругление углов -------------------------------------------------------
//
// Числом угол не спросишь, поэтому спрашиваем снимок: пиксель в самом углу
// плашки обязан остаться цветом страницы, а пиксель на той же высоте, но
// отступя вглубь, — уже подложкой. Со скруглением в ноль обе точки совпадут,
// и проверка покраснеет — что и требуется.
void checkCornersAreRound(Peek& editor) {
    const QVector<zametti::CodeBand> bands = editor.bands();
    if (bands.isEmpty()) return;
    const zametti::CodePlate plate = zametti::codePlate(editor.zoom());
    const QImage shot = editor.grab().toImage();

    for (const zametti::CodeBand& band : bands) {
        if (!band.first) continue;
        const int y = int(band.rect.top() - plate.strip) + 1;
        const int corner = int(band.rect.left()) + 1;
        const int inside = int(band.rect.left() + plate.radius + 4);
        if (y < 0 || y >= shot.height() || inside >= shot.width()) continue;
        const QRgb atCorner = shot.pixel(corner, y);
        const QRgb atInside = shot.pixel(inside, y);
        check(atCorner != atInside,
              "верхний левый угол плашки скруглён (угол " + std::to_string(qRed(atCorner)) +
                  ", внутри " + std::to_string(qRed(atInside)) + ")");
        return;   // одного угла довольно: скругление рисует один и тот же код
    }
}

// --- полоска не темнее подложки ---------------------------------------------
//
// Цвета у обеих полупрозрачные, и нарисованные одна поверх другой они дают
// удвоенную плотность: полоска выходит темнее подложки при одинаковых цветах в
// конфиге. Владелец заметил это глазами дважды подряд — спрашиваем пикселем.
void checkStripIsNotDarker(Peek& editor) {
    const QVector<zametti::CodeBand> bands = editor.bands();
    if (bands.isEmpty()) return;
    const zametti::CodePlate plate = zametti::codePlate(editor.zoom());
    const QImage shot = editor.grab().toImage();
    zametti::appearance().codeStripBackground = zametti::appearance().codeBackground;

    for (const zametti::CodeBand& band : bands) {
        if (!band.first) continue;
        // Точка в полоске и точка в теле плашки — на одной вертикали, подальше
        // от имени языка и от черты под полоской.
        const int x = int(band.rect.right() - 40);
        const int inStrip = int(band.rect.top() - plate.strip / 2);
        const int inBody = int(band.rect.top() + band.rect.height() / 2);
        if (x <= 0 || inStrip <= 0 || inBody >= shot.height()) continue;
        const QRgb strip = shot.pixel(x, inStrip);
        const QRgb body = shot.pixel(x, inBody);
        check(qAbs(qRed(strip) - qRed(body)) <= 1,
              "полоска не темнее подложки (" + std::to_string(qRed(strip)) + " против " +
                  std::to_string(qRed(body)) + ")");
        return;
    }
}

// --- на бумаге полоски нет --------------------------------------------------
//
// Имя языка и кнопка копирования — органы управления, а не содержание: на
// странице им делать нечего (решение владельца). Скруглённые углы и поля
// остаются, поэтому спрашиваем не «плашки нет», а «в полоске пусто»: полоса
// пикселей над первой строкой кода обязана быть ровной.
void checkPaperHasNoStrip(Peek& editor) {
    const QVector<zametti::CodeBand> bands = editor.bands();
    const zametti::CodePlate plate = zametti::codePlate(editor.zoom());
    for (const zametti::CodeBand& band : bands) {
        if (!band.first || band.info.isEmpty()) continue;
        const QRectF area(0, band.rect.top() - plate.strip - 4, band.rect.right() + 20,
                          plate.strip + 8);
        const QImage sheet = editor.onPaper(area);

        // Ровная — значит в каждой строке пикселей полоски не больше двух
        // разных цветов (сама плашка и поле страницы слева от неё).
        int worst = 0;
        for (int y = 5; y < int(plate.strip) - 2; ++y) {
            QSet<QRgb> colours;
            for (int x = int(band.rect.left()) + 4; x < int(band.rect.right()) - 4; ++x)
                colours.insert(sheet.pixel(x, y));
            worst = qMax(worst, colours.size());
        }
        check(worst == 1, "в полоске на бумаге пусто (цветов в строке: " +
                              std::to_string(worst) + ")");
        return;
    }
}

// Снимок с открытым полем ввода языка и серым дополнением — то, что владелец
// проверяет глазами (бриф этапа 11, часть 2).
// Поле поверх УЖЕ ВВЕДЁННОГО языка обязано закрывать его целиком: прозрачное
// поле не стирает нарисованное под ним, и буквы наезжали одна на другую
// (владелец увидел это как «фон не чистится»). Спрашиваем пикселем: под полем
// не должно остаться ни одной тёмной точки от прежней надписи.
void checkEditorHidesOldName(Peek& editor) {
    for (const zametti::CodeBand& band : editor.bands()) {
        if (!band.first || band.info.isEmpty()) continue;   // блок С языком
        const QRectF where = editor.langRect(band);
        if (where.isEmpty()) continue;
        const QRect strip = where.translated(0, -editor.verticalScrollBar()->value()).toRect();
        zametti::LanguageEditor* field = editor.editCodeLanguage(band.blockNumber, strip);
        if (field == nullptr) return;
        field->clear();   // имя стёрли — от прежнего не должно остаться следа
        QTest::qWait(40);
        const QImage shot = editor.grab().toImage();

        // Считаем не точки, а КОЛОНКИ с тёмными точками: одна-две — это
        // мигающая каретка пустого поля (её фаза от прогона к прогону разная),
        // а прежнее имя занимало бы их десятки.
        int columns = 0;
        for (int x = strip.left() + 1; x < strip.right() - 1; ++x) {
            bool dark = false;
            for (int y = strip.top() + 3; y < strip.bottom() - 3 && !dark; ++y)
                dark = qGray(shot.pixel(x, y)) < 200;
            if (dark) ++columns;
        }
        check(columns <= 2, "под полем ввода не осталось прежней надписи (тёмных колонок " +
                                std::to_string(columns) + ")");
        editor.closeCodeLanguageEditor();
        QTest::qWait(10);
        return;
    }
}

void shootLanguageEditor(Peek& editor) {
    for (const zametti::CodeBand& band : editor.bands()) {
        if (!band.first || !band.info.isEmpty()) continue;   // блок БЕЗ языка
        const QRectF where = editor.langRect(band);
        if (where.isEmpty()) continue;
        const QRect strip = where.translated(0, -editor.verticalScrollBar()->value()).toRect();
        zametti::LanguageEditor* field = editor.editCodeLanguage(band.blockNumber, strip);
        if (field == nullptr) return;
        QTest::keyClicks(field, QStringLiteral("p"));
        QTest::qWait(30);
        check(!field->completion().isEmpty(), "на снимке видно серое дополнение");
        const QImage shot = editor.grab().toImage();
        const QString path = QDir(g_shots).filePath(QStringLiteral("язык-ввод.png"));
        if (!shot.save(path)) std::printf("  НЕ СОХРАНИЛСЯ снимок ввода языка\n");
        editor.closeCodeLanguageEditor();
        QTest::qWait(10);
        return;
    }
}

void shots(int width, int height, const QString& tag, bool checks) {
    Peek editor;
    open(editor, width, height, tag);
    if (checks) {
        checkStripIsNotText(editor);
        checkPlateGeometry(editor);
        checkCornersAreRound(editor);
        checkStripIsNotDarker(editor);
        checkPaperHasNoStrip(editor);
        checkEditorHidesOldName(editor);
        shootLanguageEditor(editor);
        // Снимок — ДО проверки копирования: та оставляет на кнопке галочку
        // «скопировано», и на снимке приёмки она бы озадачивала.
        shoot(editor, tag);
        checkCopy(editor);
        return;
    }
    shoot(editor, tag);
}

}  // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    if (argc < 2) {
        std::printf("code_shots_test <куда класть снимки>\n");
        return 2;
    }
    g_shots = QString::fromLocal8Bit(argv[1]);
    QDir().mkpath(g_shots);
    g_store = QDir(g_shots).filePath(QStringLiteral("хранилище"));
    QDir(g_store).removeRecursively();
    QDir().mkpath(g_store);

    // Окно И ШИРОКОЕ, И УЗКОЕ: все прежние тестовые окна были узкие, и целый
    // класс расхождений был невидим.
    shots(1000, 700, QStringLiteral("код-широкое"), true);
    shots(640, 700, QStringLiteral("код-узкое"), false);

    std::printf("снимки: %s\n", qPrintable(g_shots));
    std::printf("блоки кода: %d проверок, %s\n", zt::g_checks,
                zt::g_failures == 0 ? "всё зелено"
                                    : (std::to_string(zt::g_failures) + " провалов").c_str());
    return zt::g_failures == 0 ? 0 : 1;
}
