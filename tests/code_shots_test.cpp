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
#include "pieces.h"
#include "document_builder.h"
#include "editor_widget.h"
#include "lang_editor.h"
#include "note_view.h"
#include "settings.h"
#include "test_util.h"

#include <vector>
#include "testdata.h"

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
    // Блок кода — один QTextBlock, и полоса у него одна на весь блок; блоков в
    // образце четыре.
    check(bands.size() == 4, "полос столько же, сколько блоков кода (" +
                                 std::to_string(bands.size()) + ")");
    if (bands.isEmpty()) return;

    const zametti::CodePlate plate = zametti::codePlate();
    check(plate.strip > 0.0, "высота полоски положительна");
    check(plate.padTop > 0.0 && plate.padTop < plate.strip,
          "поле сверху есть и меньше полоски: " + num(plate.padTop) + " < " +
              num(plate.strip));

    const QAbstractTextDocumentLayout* layout = editor.document()->documentLayout();
    int first = 0;
    for (const zametti::CodeBand& band : bands) {
        if (!band.first) continue;
        ++first;
        const QTextBlock block = editor.document()->findBlockByNumber(band.blockNumber);
        const QTextBlock before = block.previous();
        if (!before.isValid()) continue;

        // Воздух над плашкой стоит В РЕЗЕРВЕ, который сборщик отвёл верхним
        // полем блока. Если бы величины разошлись, плашка налезла бы на
        // предыдущий абзац — это и спрашиваем.
        const qreal plateTop = band.rect.top() - plate.padTop;
        const qreal aboveBottom = layout->blockBoundingRect(before).bottom();
        check(plateTop >= aboveBottom - 0.5,
              "плашка не налезает на блок выше: " + num(plateTop) + " >= " +
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

    // Последняя строка блока несёт нижнее поле — ту самую полоску, в которой
    // стоят язык и кнопка, — и плашка на нём кончается. Заодно спрашиваем, что
    // полоска не налезает на СЛЕДУЮЩИЙ блок: Qt между соседями берёт из двух
    // полей максимум, и меньше полоски зазор стать не может.
    for (const zametti::CodeBand& band : bands) {
        if (!band.last) continue;
        const QTextBlock block = editor.document()->findBlockByNumber(band.blockNumber);
        check(std::fabs(block.blockFormat().bottomMargin() - plate.strip) < 0.5,
              "полоска стоит нижним полем последней строки блока");
        const QTextBlock after = block.next();
        if (!after.isValid()) continue;
        const qreal stripBottom = band.rect.bottom() + plate.strip;
        const qreal belowTop = layout->blockBoundingRect(after).top();
        check(stripBottom <= belowTop + 0.5,
              "полоска не налезает на блок ниже: " + num(stripBottom) + " <= " +
                  num(belowTop));
    }
}

// --- резерв ставит САМ СБОРЩИК ----------------------------------------------
//
// Проверка выше смотрит на готовое окно, а там поля успел переставить обход
// syncImageSpace: сборщик мог бы ставить что угодно, и она осталась бы зелёной
// (убедился, подменив величину в сборщике). Спрашиваем сборщик напрямую — без
// вида, без обхода: полоску резервирует нижнее поле последней строки блока,
// воздух под скругление — верхнее поле первой.
void checkBuilderReservesStrip() {
    const zametti::CodePlate plate = zametti::codePlate();
    QTextDocument doc;
    zametti::buildDocument(pieces("текст\n\n```python\nx = 1\ny = 2\n```\n"), doc);

    // Блок кода — ОДИН QTextBlock: полоска висит в его нижнем поле, воздух под
    // скругление — в верхнем, а строк внутри столько, сколько в файле.
    int blocks = 0;
    int lines = 0;
    for (QTextBlock b = doc.firstBlock(); b.isValid(); b = b.next()) {
        if (zametti::isRawBlock(b) || zametti::kindOf(b) != zametti::Kind::Code) continue;
        ++blocks;
        lines += int(b.text().count(QChar::LineSeparator)) + 1;
        check(std::fabs(b.blockFormat().bottomMargin() - plate.strip) < 0.5,
              "сборщик: нижнее поле блока кода " + num(b.blockFormat().bottomMargin()) +
                  ", ждали " + num(plate.strip));
        check(b.blockFormat().topMargin() >= plate.padTop - 0.5,
              "сборщик: сверху у плашки воздух " + num(b.blockFormat().topMargin()) +
                  " >= " + num(plate.padTop));
    }
    check(blocks == 1, "блок кода в собранном документе один (" + std::to_string(blocks) + ")");
    check(lines == 2, "а строк в нём две (" + std::to_string(lines) + ")");

    // Блок кода в самом конце заметки до полоски долистывается и без своего
    // поля: нижнее поле страницы (verticalMargin, 27 px) само по себе выше
    // полоски (21 px) — замерено. Проверки на это нет НАРОЧНО: со снятой
    // починкой она оставалась зелёной, то есть спрашивала не то, что обещала.
}

// ОДИН РЕГУЛЯТОР — КЕГЛЬ ПОДПИСИ (решение владельца, refactor3): полоска и
// значок считаются от шрифта подписи; поднять кегль — выше полоска и крупнее
// значок, ничего больше трогать не надо. Стиль — копией, настройки целы.
void checkPlateFollowsLangSize() {
    const zametti::CodePlate base = zametti::codePlate();
    zametti::ZDocStyle big = zametti::settings().style();
    big.setCodeLangPointSize(zametti::settings().style().codeLangPointSize() * 2.0);
    const zametti::CodePlate grown = zametti::codePlate(big);
    check(grown.strip > base.strip, "полоска выше при крупной подписи: " + num(grown.strip) +
                                        " > " + num(base.strip));
    check(grown.iconSide > base.iconSide, "значок крупнее при крупной подписи: " +
                                              num(grown.iconSide) + " > " + num(base.iconSide));
    check(base.iconSide <= base.strip && grown.iconSide <= grown.strip,
          "значок помещается в полоску при обоих кеглях");
    // Пропорции держатся: вдвое крупнее кегль — примерно вдвое выше полоска.
    check(std::fabs(grown.strip / base.strip - 2.0) < 0.2,
          "полоска растёт вместе с кеглем: " + num(grown.strip / base.strip));
    // Верхнее поле — от строки кода, и от подписи не зависит.
    check(std::fabs(grown.padTop - base.padTop) < 0.5, "верхнее поле не меняется");
}

// --- копирование ------------------------------------------------------------
//
// Договор из брифа: в буфер уходит текст кода с переводами строк, без заборов
// и без имени языка.
void checkCopy(Peek& editor) {
    const QVector<zametti::CodeBand> bands = editor.bands();
    const zametti::CodePlate plate = zametti::codePlate();
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

    // Кнопка нарисована в полоске, а не в тексте: её прямоугольник лежит ниже
    // последней строки кода и у правого края плашки.
    for (const zametti::CodeBand& band : bands) {
        if (band.firstBlockNumber != firstBlock || !band.last) continue;
        const QRectF box = editor.button(band);
        check(!box.isEmpty(), "кнопка копирования есть");
        check(box.top() >= band.rect.bottom() - 0.5,
              "кнопка стоит под последней строкой кода");
        check(box.right() <= band.rect.right() + 0.5, "кнопка не вылезает за плашку");
        // Значок — внутри полоски и стороной от кегля подписи (codePlate).
        check(box.bottom() <= band.rect.bottom() + plate.strip + 0.5,
              "кнопка не вылезает ниже полоски");
        check(std::fabs(box.width() - std::min(plate.iconSide, plate.strip)) < 0.5,
              "сторона значка — из codePlate: " + num(box.width()) + " vs " + num(plate.iconSide));
        // И правее имени языка: оба живут в правом углу, надпись прижата к
        // кнопке слева.
        const QRectF where = editor.langRect(band);
        check(!where.isEmpty() && box.left() - where.right() >= plate.langGap - 0.5,
              "между надписью и значком просвет в codeLangGap: " +
                  num(box.left() - where.right()) + " >= " + num(plate.langGap));

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
    const zametti::CodePlate plate = zametti::codePlate();
    const QImage shot = editor.grab().toImage();
    // СНИМОК — ВСЕГО ВИДЖЕТА, а полосы кода — в координатах вьюпорта, и между
    // ними рамка QTextBrowser в один пиксель. Пока плашка стояла на целых
    // высотах, проба «на пиксель внутрь» попадала в неё и с этим сдвигом;
    // высота строки стала дробной, и проба уехала на ряд выше плашки.
    const QPoint origin = editor.viewport()->mapTo(&editor, QPoint(0, 0));

    for (const zametti::CodeBand& band : bands) {
        if (!band.first) continue;
        const int y = origin.y() + int(band.rect.top() - plate.padTop) + 1;
        const int corner = origin.x() + int(band.rect.left()) + 1;
        const int inside = origin.x() + int(band.rect.left() + plate.radius + 4);
        if (y < 0 || y >= shot.height() || inside >= shot.width()) continue;
        const QRgb atCorner = shot.pixel(corner, y);
        const QRgb atInside = shot.pixel(inside, y);
        check(atCorner != atInside,
              "верхний левый угол плашки скруглён (угол " + std::to_string(qRed(atCorner)) +
                  ", внутри " + std::to_string(qRed(atInside)) + ")");
        return;   // одного угла довольно: скругление рисует один и тот же код
    }
}

// --- полоска не отличается от подложки ---------------------------------------
//
// Полоска и подложка — одна заливка, и никакой черты между ними нет (решение
// владельца). Прежде их красили по отдельности полупрозрачными цветами, и
// нарисованные внахлёст они давали удвоенную плотность: владелец дважды видел
// «полоска чуть темнее подложки». Спрашиваем пикселем.
void checkStripIsNotDarker(Peek& editor) {
    const QVector<zametti::CodeBand> bands = editor.bands();
    if (bands.isEmpty()) return;
    const zametti::CodePlate plate = zametti::codePlate();
    const QImage shot = editor.grab().toImage();

    for (const zametti::CodeBand& band : bands) {
        if (!band.last) continue;
        // Точка в полоске и точка в теле плашки — на одной вертикали, у ЛЕВОГО
        // края: справа стоят имя языка и кнопка, и попасть в букву значило бы
        // мерить цвет надписи.
        const int x = int(band.rect.left() + 20);
        const int inStrip = int(band.rect.bottom() + plate.strip / 2);
        const int inBody = int(band.rect.top() + band.rect.height() / 2);
        if (x <= 0 || inBody <= 0 || inStrip >= shot.height()) continue;
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
// пикселей под последней строкой кода обязана быть ровной.
void checkPaperHasNoStrip(Peek& editor) {
    const QVector<zametti::CodeBand> bands = editor.bands();
    const zametti::CodePlate plate = zametti::codePlate();
    for (const zametti::CodeBand& band : bands) {
        if (!band.last || band.info.isEmpty()) continue;
        const QRectF area(0, band.rect.bottom() - 4, band.rect.right() + 20,
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
        if (!band.last || band.info.isEmpty()) continue;   // блок С языком
        const QRectF where = editor.langRect(band);
        if (where.isEmpty()) continue;
        const QRect strip = where.translated(0, -editor.verticalScrollBar()->value()).toRect();
        zametti::LanguageEditor* field = editor.editCodeLanguage(band.firstBlockNumber, strip);
        if (field == nullptr) return;
        field->clear();   // имя стёрли — от прежнего не должно остаться следа
        QTest::qWait(40);
        const QImage shot = editor.grab().toImage();

        // Считаем не точки, а КОЛОНКИ с тёмными точками: одна-две — это
        // мигающая каретка пустого поля (её фаза от прогона к прогону разная),
        // а прежнее имя занимало бы их десятки.
        // Смотрим ВСЮ полоску, без отступов по краям: первая редакция этой
        // проверки щадила по три пикселя сверху и снизу, и остатки прежней
        // надписи, торчащие у самых кромок, оставались невидимыми для неё.
        int columns = 0;
        for (int x = strip.left(); x <= strip.right(); ++x) {
            bool dark = false;
            for (int y = strip.top(); y <= strip.bottom() && !dark; ++y)
                dark = qGray(shot.pixel(x, y)) < 200;
            if (dark) ++columns;
        }
        check(columns <= 2, "под полем ввода не осталось прежней надписи (тёмных колонок " +
                                std::to_string(columns) + ")");

        // И ГЛАВНОЕ: под полем не нарисовано НИЧЕГО — вид не рисует свою
        // надпись, пока язык правят. Прячем поле и смотрим на голую полоску:
        // так проверка не зависит ни от стиля, ни от того, точно ли поле
        // попало в прямоугольник надписи.
        field->hide();
        QTest::qWait(40);
        const QImage bare = editor.grab().toImage();
        int left = 0;
        for (int x = strip.left(); x <= strip.right(); ++x)
            for (int y = strip.top(); y <= strip.bottom(); ++y)
                if (qGray(bare.pixel(x, y)) < 200) ++left;
        check(left == 0, "пока язык правят, вид своей надписи не рисует (точек " +
                             std::to_string(left) + ")");
        editor.closeCodeLanguageEditor();
        QTest::qWait(10);
        return;
    }
}

void shootLanguageEditor(Peek& editor) {
    for (const zametti::CodeBand& band : editor.bands()) {
        if (!band.last || !band.info.isEmpty()) continue;   // блок БЕЗ языка
        const QRectF where = editor.langRect(band);
        if (where.isEmpty()) continue;
        const QRect strip = where.translated(0, -editor.verticalScrollBar()->value()).toRect();
        zametti::LanguageEditor* field = editor.editCodeLanguage(band.firstBlockNumber, strip);
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

// --- полоска переживает частичную перерисовку --------------------------------
//
// При прокрутке Qt перерисовывает не весь вьюпорт, а открывшуюся полосу. Она
// запросто попадает целиком в ПОЛЕ блока — туда, где нарисована полоска, а не
// текст, — и по голому прямоугольнику блока такой кусок оказывался невидимым:
// полоска то рисовалась, то нет (нашёл владелец; та же беда была у рамок
// вокруг картинок).
//
// Проверка перерисовывает ровно такую полосу и смотрит, что в ней нарисовано.
void checkStripSurvivesPartialRepaint(Peek& editor) {
    const QVector<zametti::CodeBand> bands = editor.bands();
    const zametti::CodePlate plate = zametti::codePlate();
    for (const zametti::CodeBand& band : bands) {
        if (!band.last || band.info.isEmpty()) continue;
        const int top = int(band.rect.bottom()) - editor.verticalScrollBar()->value();
        if (top < 0 || top + int(plate.strip) >= editor.viewport()->height()) continue;

        // Полоса РОВНО в высоту полоски: текста блока в ней нет ни пикселя.
        const QRect slice(0, top + 1, editor.viewport()->width(), int(plate.strip) - 2);
        QImage sheet(editor.viewport()->size(), QImage::Format_RGB32);
        sheet.fill(Qt::magenta);   // чтобы нетронутое было видно сразу
        editor.viewport()->render(&sheet, slice.topLeft(), QRegion(slice));

        const int x = int(band.rect.left()) + 20;
        const int y = slice.top() + slice.height() / 2;
        const QRgb painted = sheet.pixel(x, y);
        check(painted != qRgb(255, 0, 255), "полоса перерисовалась вообще");
        // Плашка не равна фону страницы — по этому её и опознаём.
        const QColor page = zametti::settings().style().pageBackground();
        check(qAbs(qRed(painted) - page.red()) > 1 || qAbs(qGreen(painted) - page.green()) > 1,
              "в перерисованной полосе есть плашка, а не голый фон (" +
                  std::to_string(qRed(painted)) + " против " + std::to_string(page.red()) + ")");
        return;
    }
}

// --- дополнение стоит ровно там, где продолжился бы набор ---------------------
//
// Владелец: набрал «c», а серое «pp» нарисовалось со сдвигом влево — вторая «p»
// и «c» слились почти в одну букву. Причина в том, что Qt отдаёт под каретку
// прямоугольник шириной десять пикселей, посаженный серединой на позицию
// каретки; его левый край — это пять пикселей влево от места набора.
//
// Сравниваем ДВА рисунка одного и того же поля: «c» + серое «pp» и просто
// «cpp». Правый край надписи обязан совпасть — на глаз такое расхождение
// ловится плохо, а числом видно сразу.
void checkCompletionSitsAtCaret(Peek& editor) {
    const auto rightEdge = [](zametti::LanguageEditor& field) {
        QImage sheet(field.size(), QImage::Format_RGB32);
        sheet.fill(Qt::white);
        field.render(&sheet);
        int right = -1;
        for (int x = 0; x < sheet.width(); ++x)
            for (int y = 0; y < sheet.height(); ++y)
                if (qGray(sheet.pixel(x, y)) < 230) right = qMax(right, x);
        return right;
    };

    zametti::LanguageEditor whole({}, QStringLiteral("cpp"), editor.viewport());
    whole.setFont(zametti::codeLangFont());
    whole.resize(120, 20);
    // Каретку уводим в начало: её столбик правый край не сдвинет.
    whole.setCursorPosition(0);
    const int wholeRight = rightEdge(whole);

    zametti::LanguageEditor typed({QStringLiteral("cpp")}, QString(), editor.viewport());
    typed.setFont(zametti::codeLangFont());
    typed.resize(120, 20);
    QTest::keyClicks(&typed, QStringLiteral("c"));
    QTest::qWait(10);
    check(typed.completion() == QStringLiteral("pp"), "дополнилось «pp»");
    const int typedRight = rightEdge(typed);

    check(qAbs(typedRight - wholeRight) <= 1,
          "серый хвост кончается там же, где кончилось бы набранное целиком (" +
              std::to_string(typedRight) + " против " + std::to_string(wholeRight) + ")");
    whole.hide();
    typed.hide();
}

void shots(int width, int height, const QString& tag, bool checks) {
    Peek editor;
    open(editor, width, height, tag);
    if (checks) {
        checkStripIsNotText(editor);
        checkPlateGeometry(editor);
        checkBuilderReservesStrip();
    checkPlateFollowsLangSize();
        checkCornersAreRound(editor);
        checkStripIsNotDarker(editor);
        checkPaperHasNoStrip(editor);
        checkStripSurvivesPartialRepaint(editor);
        checkCompletionSitsAtCaret(editor);
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

static int ztRunSuite(int argc, char** argv) {
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
    return zt::freshFailures();
}

// Набор целиком одним TEST: тело не тронуто, argv ему собран здесь.
// Дробить на отдельные проверки — отдельная работа, по одному набору.
TEST(CodeShots, All) {
    std::vector<QByteArray> ztArgs{QByteArrayLiteral("code_shots_test")};
    ztArgs.push_back((zt::TestData::outDir(QStringLiteral("code-shots"))).toLocal8Bit());
    std::vector<char*> ztArgv;
    for (QByteArray& a : ztArgs) ztArgv.push_back(a.data());
    EXPECT_EQ(0, ztRunSuite(int(ztArgv.size()), ztArgv.data()));
}
