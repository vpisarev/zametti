// Снимки выключных формул: то, что владелец проверяет глазами, и то немногое,
// что проверяется по снимку числом.
//
// Формула — третье воплощение слоя объекта, и устроена она как показ картинки:
// строка исходника закрашивается фоном, вёрстка встаёт на её место. Значит и
// беды у неё те же, что были у картинок: наползание из-за неверного резерва,
// «текст проступает из-под вёрстки», «после правки вёрстка не вернулась».
// Каждая из них здесь и спрашивается.

#include "block_object.h"
#include "doc_model.h"
#include "editor_widget.h"
#include "formula.h"
#include "note_view.h"
#include "settings.h"
#include "test_util.h"

#include <vector>
#include "testdata.h"

#include <QAbstractTextDocumentLayout>
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFontInfo>
#include <QFontMetricsF>
#include <QImage>
#include <QScrollBar>
#include <QTest>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextLayout>

namespace {

QString g_dir;

const char* kNote = R"(# Формулы

Обычный абзац перед формулой.

$$\frac{a}{b}$$

Абзац между формулами. Дальше многострочная выключная.

$$\begin{aligned}
  (x-y)^2 &= (x-y)(x-y) \\
  &= x^2 -2xy +y^2
\end{aligned}$$

И ещё одна, с корнем и суммой:

$$\sqrt{\sum_{k=0}^\infty \frac{x^k}{k!}}$$

Хвост заметки.
)";

// Первый блок-формула документа; -1 — ни одной.
int firstFormula(zametti::NoteEditor& editor) {
    for (QTextBlock b = editor.document()->firstBlock(); b.isValid(); b = b.next())
        if (editor.formulaAt(b.blockNumber()) != nullptr) return b.blockNumber();
    return -1;
}

// Сколько тёмных точек в этом прямоугольнике снимка.
// СНИМОК — В ФИЗИЧЕСКИХ ТОЧКАХ, а прямоугольники — в логических. На экране
// с плотностью 2 это разные числа вдвое, и первая редакция считала чернила не
// там, где рисовала: проверка краснела на ровном месте, а я чуть не пошёл чинить
// отрисовку. Плотность берётся у самого снимка.
// Снимок берётся у ВСЕГО виджета, а прямоугольник вёрстки — в координатах
// документа. Между ними стоит вьюпорт, и в широком окне он СДВИНУТ: колонка
// текста центрируется полями вьюпорта (NoteView::applyContentWidth). Без этой
// поправки проверка мерила пустое поле слева и находила ноль чернил там, где
// формула нарисована прекрасно.
QPoint viewportOrigin(const zametti::NoteEditor& editor) {
    return editor.viewport()->mapTo(&const_cast<zametti::NoteEditor&>(editor), QPoint(0, 0));
}

int inkIn(const QImage& shot, const QRectF& box, int scroll, qreal dpr,
          QPoint origin = QPoint(0, 0)) {
    int dark = 0;
    const QRectF at((box.left() + origin.x()) * dpr, (box.top() - scroll + origin.y()) * dpr,
                    box.width() * dpr, box.height() * dpr);
    for (int x = int(at.left()); x < int(at.right()) && x < shot.width(); ++x)
        for (int y = int(at.top()); y < int(at.bottom()) && y < shot.height(); ++y)
            if (x >= 0 && y >= 0 && qGray(shot.pixel(x, y)) < 128) ++dark;
    return dark;
}

zametti::NoteEditor* openNote(const QString& name, int width, int height, const char* text) {
    const QString path = QDir(g_dir).filePath(name + QStringLiteral(".md"));
    QFile file(path);
    if (file.open(QIODevice::WriteOnly)) file.write(text);
    file.close();

    auto* editor = new zametti::NoteEditor;
    editor->resize(width, height);
    editor->show();
    QTest::qWait(20);
    editor->openFile(path);
    QTest::qWait(80);
    QTextCursor at = editor->textCursor();
    at.setPosition(0);
    editor->setTextCursor(at);
    QTest::qWait(60);
    return editor;
}

// --- вёрстка на месте исходника ---------------------------------------------

void checkRendered(int width, const QString& name) {
    zametti::NoteEditor* editor = openNote(name, width, 760, kNote);
    const QImage shot = editor->grab().toImage();
    if (!shot.save(QDir(g_dir).filePath(name + QStringLiteral(".png"))))
        std::printf("НЕ СОХРАНИЛСЯ снимок %s\n", qPrintable(name));

    // Все три формулы показаны вёрсткой.
    int rendered = 0;
    for (QTextBlock b = editor->document()->firstBlock(); b.isValid(); b = b.next())
        if (editor->formulaAt(b.blockNumber()) != nullptr) ++rendered;
    ZT_EQ("формул показано вёрсткой (" + name.toStdString() + ")", "3",
          std::to_string(rendered));

    const int first = firstFormula(*editor);
    if (first < 0) {
        ++zt::g_checks;
        ++zt::g_failures;
        std::printf("провал: ни одной формулы не показано\n");
        delete editor;
        return;
    }

    // ВНУТРИ ПРЯМОУГОЛЬНИКА ВЁРСТКИ ЧТО-ТО НАРИСОВАНО. Проверка не придирка:
    // ровно так выглядит формула, отрисованная нулевым кеглем или прозрачным
    // цветом, — движок при этом не жалуется.
    const QRectF box = editor->formulaRect(first);
    const int scroll = editor->verticalScrollBar()->value();
    std::printf("[замер] dpr=%.2f кегль=%d высота строки=%.1f вёрстка=%.1fx%.1f\n",
                editor->devicePixelRatioF(), QFontInfo(editor->baseFont()).pixelSize(),
                QFontMetricsF(editor->baseFont()).height(), box.width(), box.height());
    ZT_TRUE("у вёрстки есть размер: " + std::to_string(int(box.width())) + "x" +
                std::to_string(int(box.height())),
            box.width() > 4 && box.height() > 4);
    const int ink = inkIn(shot, box, scroll, editor->devicePixelRatioF(), viewportOrigin(*editor));
    const qreal dprNow = editor->devicePixelRatioF();
    const qreal area = box.width() * dprNow * box.height() * dprNow;
    std::printf("[замер] чернил %d из %d точек (%.0f%%)\n", ink, int(area), 100.0 * ink / area);
    ZT_TRUE("в прямоугольнике вёрстки есть чернила", ink > 20);

    // ВЁРСТКА ПО ЦЕНТРУ КОЛОНКИ. Слева от неё поле, справа поле, и они близки.
    const qreal leftGap = box.left();
    const qreal rightGap = editor->viewport()->width() - box.right();
    ZT_TRUE("вёрстка по центру: слева " + std::to_string(int(leftGap)) + ", справа " +
                std::to_string(int(rightGap)),
            std::abs(leftGap - rightGap) < 0.25 * editor->viewport()->width());

    // ИСХОДНИКА НА ЭКРАНЕ НЕТ ВОВСЕ. Он не закрашен фоном поверх — он погашен:
    // рисовать текст, чтобы тут же закрыть его картинкой, незачем, и следы
    // `$$…$$` при прокрутке брались именно оттуда (владелец: «зачем ты
    // рендеришь latex-текст, а потом стираешь фон и рисуешь поверх формулу?»).
    // Полоса СЛЕВА от вёрстки — там, где начинался бы текст, — обязана быть
    // чистой.
    if (leftGap > 12) {
        const QRectF stripe(0, box.top(), leftGap - 4, box.height());
        ZT_TRUE("слева от вёрстки исходник не проступает",
                inkIn(shot, stripe, scroll, editor->devicePixelRatioF(), viewportOrigin(*editor)) == 0);
    }

    // ВЁРСТКА НЕ ВЫЛЕЗАЕТ ЗА СВОЙ ПРЯМОУГОЛЬНИК. Полоса сразу справа от неё
    // обязана быть чистой. Проверка не придирка: на экране с плотностью 2
    // формула рисовалась вдвое крупнее и уезжала за колонку — владелец увидел
    // «на экран влезает только кусок». Числа при этом были верные, врала
    // отрисовка, и поймать это можно только по снимку.
    {
        const qreal right = editor->viewport()->width() - box.right() - 4;
        if (right > 8) {
            const QRectF beyond(box.right() + 2, box.top(), right, box.height());
            ZT_TRUE("справа от вёрстки чисто",
                    inkIn(shot, beyond, scroll, editor->devicePixelRatioF(), viewportOrigin(*editor)) == 0);
        }
    }

    // ФОРМУЛЫ НЕ НАПОЛЗАЮТ ДРУГ НА ДРУГА. Между соседними прямоугольниками
    // вёрстки должен быть зазор — это та самая беда резерва, которая у картинок
    // ловилась только глазами и только на повторном открытии.
    std::vector<QRectF> boxes;
    for (QTextBlock b = editor->document()->firstBlock(); b.isValid(); b = b.next())
        if (editor->formulaAt(b.blockNumber()) != nullptr)
            boxes.push_back(editor->formulaRect(b.blockNumber()));
    for (size_t i = 1; i < boxes.size(); ++i)
        ZT_TRUE("формулы " + std::to_string(i) + " и " + std::to_string(i + 1) +
                    " не наползают",
                boxes[i].top() >= boxes[i - 1].bottom() - 1.0);

    delete editor;
}

// --- выбранная формула видна уголками ---------------------------------------
//
// Слой объекта у формулы был, а на экране его не было: каретка, вставшая на
// формулу, ничем не отличалась от каретки в тексте. Владелец так и прочитал —
// «ничего из этого не работает». Уголки те же, что у фотографии и у таблицы.
void checkCornersOnSelected() {
    zametti::NoteEditor* editor =
        openNote(QStringLiteral("уголки"), 900, 700, "до\n\n$$x^2 + y^2 = z^2$$\n\nпосле\n");
    const int first = firstFormula(*editor);
    ZT_TRUE("формула показана", first >= 0);
    if (first < 0) {
        delete editor;
        return;
    }
    const QRectF box = editor->formulaRect(first);
    const int scroll = editor->verticalScrollBar()->value();
    const qreal dpr = editor->devicePixelRatioF();

    // Каретка в тексте — уголков нет. Считаем точки цвета каретки в рамке
    // вокруг вёрстки: именно им уголки и рисуются.
    const auto cornerInk = [&](const QImage& shot) {
        const QColor mark = zametti::settings().style().caretColor();
        int hits = 0;
        const QRectF around(box.adjusted(-24, -24, 24, 24));
        for (int x = int(around.left() * dpr); x < int(around.right() * dpr) && x < shot.width();
             ++x)
            for (int y = int((around.top() - scroll) * dpr);
                 y < int((around.bottom() - scroll) * dpr) && y < shot.height(); ++y) {
                if (x < 0 || y < 0) continue;
                const QColor at = shot.pixelColor(x, y);
                if (std::abs(at.red() - mark.red()) < 40 && std::abs(at.green() - mark.green()) < 40 &&
                    std::abs(at.blue() - mark.blue()) < 40)
                    ++hits;
            }
        return hits;
    };

    editor->setTextCursor(QTextCursor(editor->document()->firstBlock()));
    QTest::qWait(40);
    const int without = cornerInk(editor->grab().toImage());

    editor->setTextCursor(QTextCursor(editor->document()->findBlockByNumber(first)));
    QTest::qWait(40);
    const QImage shot = editor->grab().toImage();
    const int with = cornerInk(shot);
    if (!shot.save(QDir(g_dir).filePath(QStringLiteral("уголки-выбрана.png"))))
        std::printf("НЕ СОХРАНИЛСЯ снимок уголков\n");

    ZT_TRUE("без каретки на формуле уголков нет: " + std::to_string(without), without < 20);
    ZT_TRUE("с кареткой на формуле уголки видны: " + std::to_string(with), with > 60);

    // И САМОЙ КАРЕТКИ ТАМ НЕТ. Выбранную формулу показывают уголки, а не
    // мигающая полоска посреди вёрстки. Ровно та же беда была у таблицы, и это
    // третья её копия — потому вопрос теперь один на все объекты
    // (caretShouldBeDrawn принимает «объект показан вместо исходника»).
    ZT_TRUE("на нарисованной формуле каретка не рисуется",
            !zametti::caretShouldBeDrawn(true, false, false, true));
    ZT_TRUE("а в тексте рисуется", zametti::caretShouldBeDrawn(true, false, false, false));

    delete editor;
}

// --- вёрстка помещается в СВОЙ блок ------------------------------------------
//
// Владелец: «рендеринг оставляет мусор, особенно при прокрутке — как будто
// размеры боксов посчитаны неверно». Так и есть, если картинка формулы выше
// того места, которое ей отвёл Qt: рисуем мы за границей блока, а перерисовку
// при прокрутке Qt заказывает по границе. Значит проверять надо не «красиво
// ли», а именно это: вёрстка обязана помещаться в прямоугольник своего блока
// вместе с его нижним полем.
void checkFormulaFitsItsBlock() {
    zametti::NoteEditor* editor = openNote(QStringLiteral("габариты"), 900, 760, kNote);
    int checked = 0;
    for (QTextBlock b = editor->document()->firstBlock(); b.isValid(); b = b.next()) {
        const zametti::FormulaRender* render = editor->formulaAt(b.blockNumber());
        if (render == nullptr) continue;
        const QRectF box = editor->formulaRect(b.blockNumber());
        const QRectF area = editor->document()->documentLayout()->blockBoundingRect(b);
        const qreal bottom = area.bottom() + b.blockFormat().bottomMargin();
        ZT_TRUE("вёрстка не вылезает вниз за свой блок: низ " +
                    std::to_string(int(box.bottom())) + ", блок до " + std::to_string(int(bottom)),
                box.bottom() <= bottom + 1.0);
        ZT_TRUE("и не начинается выше него: верх " + std::to_string(int(box.top())) +
                    ", блок с " + std::to_string(int(area.top())),
                box.top() >= area.top() - 1.0);
        ++checked;
    }
    ZT_TRUE("проверено формул: " + std::to_string(checked), checked >= 3);

    // И БЛОК НЕ ВЫШЕ САМОЙ ВЁРСТКИ ПЛЮС ОТБИВКА. Резерв, взятый с запасом,
    // разгоняет заметку: между формулами появляются огромные пустые полосы.
    // Проверки на это не было вовсе — владелец увидел её отсутствие раньше,
    // чем я. Мера отбивки — строка текста: больше строки пустоты под формулой
    // человек читает как дыру. Мера — вёрстка плюс отбивка абзаца (у нас это
    // около полутора строк): больше — уже полоса пустоты.
    const qreal lineHeight = QFontMetricsF(editor->baseFont()).height();
    for (QTextBlock b = editor->document()->firstBlock(); b.isValid(); b = b.next()) {
        const zametti::FormulaRender* render = editor->formulaAt(b.blockNumber());
        if (render == nullptr) continue;
        const QRectF area = editor->document()->documentLayout()->blockBoundingRect(b);
        const qreal total = area.height() + b.blockFormat().bottomMargin();
        ZT_TRUE("блок формулы не раздут: " + std::to_string(int(total)) + " при вёрстке " +
                    std::to_string(int(render->height)) + " и строке " +
                    std::to_string(int(lineHeight)),
                total <= render->height + 2.0 * lineHeight + 2.0);
    }
    delete editor;
}

// --- буква на выбранной формуле в неё не попадает ---------------------------
//
// Владелец: «выделив формулу рамочкой, удаётся нажать букву, и иногда она
// вставляется прямо в формулу — хотя договаривались, что формула правится
// только как latex-исходник». Правило одно на все объекты: всё, чего слой
// объекта не назвал действием, на объекте не делается вовсе.
void checkTypingDoesNotEnterFormula() {
    zametti::NoteEditor* editor =
        openNote(QStringLiteral("буква"), 900, 700, "до\n\n$$x^2$$\n\nпосле\n");
    const int first = firstFormula(*editor);
    ZT_TRUE("формула показана", first >= 0);
    if (first < 0) {
        delete editor;
        return;
    }
    const QString before = editor->document()->toPlainText();
    editor->setTextCursor(QTextCursor(editor->document()->findBlockByNumber(first)));
    QTest::qWait(20);
    QTest::keyClicks(editor, QStringLiteral("abc"));
    QTest::qWait(30);
    ZT_EQ("буквы в формулу не попали", before.toStdString(),
          editor->document()->toPlainText().toStdString());
    ZT_TRUE("и вёрстка на месте", firstFormula(*editor) >= 0);

    // А по Enter — правится, и там буквы уже свои.
    QTest::keyClick(editor, Qt::Key_Return);
    QTest::qWait(30);
    QTest::keyClicks(editor, QStringLiteral("+1"));
    QTest::qWait(30);
    ZT_TRUE("в правке буквы попадают в исходник",
            editor->document()->toPlainText().contains(QStringLiteral("+1")));
    delete editor;
}

// --- выделение и двойной щелчок не проявляют исходник ------------------------
//
// Владелец: «по двойному щелчку появляются странные буквы сбоку, каждый раз
// разные». Разные — потому что двойной щелчок выделяет каждый раз своё слово, а
// ВЫДЕЛЕННЫЙ текст Qt рисует своим цветом, не спрашивая наш: погашенный
// исходник проступает из-под вёрстки.
void checkSelectionDoesNotRevealSource() {
    zametti::NoteEditor* editor = openNote(QStringLiteral("выделение"), 900, 700,
                                           "до\n\n$$\\frac{a}{b}$$\n\nпосле\n");
    const int first = firstFormula(*editor);
    ZT_TRUE("формула показана", first >= 0);
    if (first < 0) {
        delete editor;
        return;
    }
    const QRectF box = editor->formulaRect(first);
    const int scroll = editor->verticalScrollBar()->value();

    // Выделяем весь блок формулы — как это делает мышь протяжкой.
    QTextCursor at(editor->document()->findBlockByNumber(first));
    at.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
    editor->setTextCursor(at);
    QTest::qWait(40);
    const QImage shot = editor->grab().toImage();
    if (!shot.save(QDir(g_dir).filePath(QStringLiteral("выделение.png"))))
        std::printf("НЕ СОХРАНИЛСЯ снимок выделения\n");

    // ИСХОДНИКА ПОД ВЫДЕЛЕНИЕМ НЕТ — и теперь это не вопрос пикселей, а вопрос
    // строения: формула лежит в документе ОБЪЕКТОМ, одним знаком U+FFFC, и
    // проступать под выделением просто нечему. Прежняя проверка считала тёмные
    // точки справа от вёрстки; с объектом она мерила бы саму заливку выделения
    // (полоса выбрана целиком, как и у выбранной фотографии), то есть отвечала
    // бы не на тот вопрос.
    //
    // Утверждение сильнее прежнего: не «исходник не виден», а «его нет».
    ZT_TRUE("под выделением исходнику взяться неоткуда: в блоке объект",
            editor->document()->findBlockByNumber(first).text() ==
                QString(QChar::ObjectReplacementCharacter));


    // ДВОЙНОЙ ЩЕЛЧОК ПО ФОРМУЛЕ — это правка её исходника, как у таблицы, а не
    // выделение слова в невидимом тексте.
    const QPoint middle(int(box.center().x()), int(box.center().y()) - scroll);
    QTest::mouseDClick(editor->viewport(), Qt::LeftButton, Qt::NoModifier, middle);
    QTest::qWait(60);
    // Раскрытая формула — обычный абзац с исходником; своего признака «её
    // сейчас правят» больше нет, и спрашивать надо сам документ.
    ZT_TRUE("двойной щелчок открыл правку",
            editor->document()->findBlockByNumber(first).text().startsWith(
                QStringLiteral("$$")));
    ZT_TRUE("и выделения в ней не осталось", !editor->textCursor().hasSelection());

    delete editor;
}

// --- набор формул с клавиатуры ----------------------------------------------

void checkMathHotkeys() {
    zametti::NoteEditor* editor =
        openNote(QStringLiteral("клавиши"), 900, 700, "первая строка\n\nвторая строка\n");

    // Ctrl+M на выделении — обернуть в доллары; ещё раз — развернуть.
    QTextCursor at(editor->document()->firstBlock());
    at.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
    editor->setTextCursor(at);
    QTest::keyClick(editor, Qt::Key_M, Qt::ControlModifier);
    QTest::qWait(40);
    ZT_TRUE("Ctrl+M обернул выделение: " +
                editor->document()->firstBlock().text().toStdString(),
            editor->document()->firstBlock().text() == QStringLiteral("$первая строка$"));

    at = QTextCursor(editor->document()->firstBlock());
    at.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
    editor->setTextCursor(at);
    QTest::keyClick(editor, Qt::Key_M, Qt::ControlModifier);
    QTest::qWait(40);
    ZT_TRUE("и второй раз развернул обратно",
            editor->document()->firstBlock().text() == QStringLiteral("первая строка"));

    // Ctrl+Shift+M на абзаце — выключная формула целым блоком.
    QTextCursor line(editor->document()->findBlockByNumber(2));
    editor->setTextCursor(line);
    QTest::keyClick(editor, Qt::Key_M, Qt::ControlModifier | Qt::ShiftModifier);
    QTest::qWait(80);
    const QString made = editor->document()->findBlockByNumber(2).text();
    ZT_TRUE("Ctrl+Shift+M сделал выключную: " + made.toStdString(),
            made == QStringLiteral("$$вторая строка$$"));

    delete editor;
}

// --- флип: Enter показывает исходник, уход каретки — вёрстку ----------------

void checkFlip() {
    zametti::NoteEditor* editor =
        openNote(QStringLiteral("флип"), 900, 700, "до\n\n$$x^2 + y^2 = z^2$$\n\nпосле\n");

    const int first = firstFormula(*editor);
    ZT_TRUE("формула показана вёрсткой", first >= 0);
    if (first < 0) {
        delete editor;
        return;
    }

    // Каретка на формулу — и Enter показывает исходник.
    editor->setTextCursor(QTextCursor(editor->document()->findBlockByNumber(first)));
    QTest::qWait(20);
    QTest::keyClick(editor, Qt::Key_Return);
    QTest::qWait(40);
    // РАСКРЫТАЯ ФОРМУЛА — ЭТО ОБЫЧНЫЙ АБЗАЦ С ИСХОДНИКОМ, а не блок с
    // признаком «его сейчас правят». Признака больше нет и не нужно: спросить
    // можно сам документ, а прежний editedFormula_ жил лишь потому, что
    // исходник лежал в блоке всегда, погашенный прозрачным цветом.
    ZT_TRUE("Enter открыл исходник",
            editor->document()->findBlockByNumber(first).text() ==
                QStringLiteral("$$x^2 + y^2 = z^2$$"));
    ZT_TRUE("вёрстки на время правки нет", editor->formulaAt(first) == nullptr);

    // ИСХОДНИК ВИДЕН ЦЕЛИКОМ. Пока он правится, закрашивать строку нечем — и
    // если бы закраска осталась, человек правил бы вслепую.
    {
        const QImage shot = editor->grab().toImage();
        const QTextBlock block = editor->document()->findBlockByNumber(first);
        const QRectF line =
            editor->document()->documentLayout()->blockBoundingRect(block);
        ZT_TRUE("исходник виден",
                inkIn(shot, line, editor->verticalScrollBar()->value(),
                      editor->devicePixelRatioF(), viewportOrigin(*editor)) > 10);
    }

    // Увели каретку наружу — снова вёрстка.
    editor->setTextCursor(QTextCursor(editor->document()->firstBlock()));
    QTest::qWait(60);
    ZT_TRUE("уход каретки закрыл правку",
            editor->document()->findBlockByNumber(first).text().size() == 1);
    ZT_TRUE("вёрстка вернулась", firstFormula(*editor) >= 0);

    delete editor;
}

// --- правка исходника: формула ожила -----------------------------------------

void checkEditChangesFormula() {
    zametti::NoteEditor* editor =
        openNote(QStringLiteral("правка"), 900, 700, "до\n\n$$x^2$$\n\nпосле\n");
    const int first = firstFormula(*editor);
    ZT_TRUE("формула показана", first >= 0);
    if (first < 0) {
        delete editor;
        return;
    }
    const QRectF before = editor->formulaRect(first);

    // Enter — правка, дописали множитель, ушли.
    editor->setTextCursor(QTextCursor(editor->document()->findBlockByNumber(first)));
    QTest::keyClick(editor, Qt::Key_Return);
    QTest::qWait(30);
    QTextCursor at = editor->textCursor();
    at.setPosition(at.block().position() + at.block().text().size() - 2);
    editor->setTextCursor(at);
    QTest::keyClicks(editor, QStringLiteral(" + y^2"));
    QTest::qWait(30);
    editor->setTextCursor(QTextCursor(editor->document()->firstBlock()));
    QTest::qWait(80);

    const int now = firstFormula(*editor);
    ZT_TRUE("после правки формула снова показана вёрсткой", now >= 0);
    if (now >= 0) {
        const QRectF after = editor->formulaRect(now);
        // Формула стала длиннее — значит рисуется НОВЫЙ исходник, а не старая
        // картинка из кэша. Без этой проверки кэш мог бы держать прошлое
        // изображение вечно, и правка не была бы видна.
        ZT_TRUE("вёрстка поменялась: было " + std::to_string(int(before.width())) +
                    ", стало " + std::to_string(int(after.width())),
                after.width() > before.width() + 4);
    }

    // И В ФАЙЛ УШЁЛ ИСХОДНИК, а не то, что нарисовано.
    editor->save(false, true);   // force: за нас признак «изменён» никто не поднимал
    QTest::qWait(40);
    QFile file(QDir(g_dir).filePath(QStringLiteral("правка.md")));
    if (file.open(QIODevice::ReadOnly)) {
        const QString text = QString::fromUtf8(file.readAll());
        ZT_TRUE("в файле дословный исходник: " + text.toStdString(),
                text.contains(QStringLiteral("$$x^2 + y^2$$")));
    } else {
        ++zt::g_checks;
        ++zt::g_failures;
        std::printf("провал: файл заметки не открылся\n");
    }

    delete editor;
}

// --- битая формула: рамка, а не огрызок --------------------------------------

void checkBroken() {
    zametti::NoteEditor* editor =
        openNote(QStringLiteral("битая"), 900, 700, "до\n\n$$\\frac{a}{b$$\n\nпосле\n");
    const int first = firstFormula(*editor);
    ZT_TRUE("битая формула тоже занимает место объекта", first >= 0);
    if (first >= 0) {
        const zametti::FormulaRender* render = editor->formulaAt(first);
        ZT_TRUE("и помечена ошибкой: " +
                    (render == nullptr ? std::string("нет вёрстки")
                                       : render->error.toStdString()),
                render != nullptr && !render->error.isEmpty());
    }
    const QImage shot = editor->grab().toImage();
    if (!shot.save(QDir(g_dir).filePath(QStringLiteral("битая.png"))))
        std::printf("НЕ СОХРАНИЛСЯ снимок битой формулы\n");
    delete editor;
}

// --- движок зовётся один раз на формулу ---------------------------------------

void checkEngineCalls() {
    zametti::Formulas::resetRenders();
    zametti::NoteEditor* editor =
        openNote(QStringLiteral("счётчик"), 900, 700, "до\n\n$$x^2$$\n\nпосле\n");
    const int after = zametti::Formulas::renders();
    ZT_TRUE("на одну формулу рендеров немного: " + std::to_string(after),
            after >= 1 && after <= 3);

    // КЭШ ВЫКЛЮЧЕН — временно, по решению владельца (проверяем догадку, не от
    // него ли странности, которых набор не ловит). Пока он выключен, повторная
    // сверка ЗОВЁТ движок, и проверка спрашивает ровно это: ослаблять её до
    // «не больше чем» значило бы заглушить набор, а включённый обратно кэш она
    // тогда покраснеет и напомнит о себе.
    // Точное число здесь не спрашивается, и это не ослабление порога, а
    // другой факт: сверок вид делает столько, сколько ему нужно (на правку
    // полей, на смену ширины), и с выключенным кэшем каждая из них зовёт
    // движок. Когда кэш вернётся, эта проверка обязана смениться на строгую —
    // «повторная сверка движок не зовёт ВОВСЕ».
    const int before = zametti::Formulas::renders();
    editor->syncFormulas();
    QTest::qWait(20);
    ZT_TRUE("с выключенным кэшем сверка зовёт движок заново",
            zametti::Formulas::renders() > before);
    delete editor;
}

}  // namespace

static int ztRunSuite(int argc, char** argv) {
    g_dir = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QDir::tempPath();
    QDir().mkpath(g_dir);

    QString error;
    if (!zametti::Formulas::init(&error)) {
        std::printf("движок формул не поднялся: %s\n", qPrintable(error));
        ++zt::g_checks;
        ++zt::g_failures;
        return zt::report("снимки формул");
    }

    // Окно и широкое, и узкое: целый класс расхождений виден только в узком
    // (правило проекта, оплаченное таблицей, уехавшей за правый край).
    checkRendered(1000, QStringLiteral("формулы-широкое"));
    checkRendered(620, QStringLiteral("формулы-узкое"));
    checkCornersOnSelected();
    checkFormulaFitsItsBlock();
    checkMathHotkeys();
    checkSelectionDoesNotRevealSource();
    checkTypingDoesNotEnterFormula();
    checkFlip();
    checkEditChangesFormula();
    checkBroken();
    checkEngineCalls();

    std::printf("снимки: %s\n", qPrintable(g_dir));
    return zt::report("снимки формул");
}

// Набор целиком одним TEST: тело не тронуто, argv ему собран здесь.
// Дробить на отдельные проверки — отдельная работа, по одному набору.
TEST(FormulaShots, All) {
    std::vector<QByteArray> ztArgs{QByteArrayLiteral("formula_shots_test")};
    ztArgs.push_back((zt::TestData::outDir(QStringLiteral("formula-shots"))).toLocal8Bit());
    std::vector<char*> ztArgv;
    for (QByteArray& a : ztArgs) ztArgv.push_back(a.data());
    EXPECT_EQ(0, ztRunSuite(int(ztArgv.size()), ztArgv.data()));
}
