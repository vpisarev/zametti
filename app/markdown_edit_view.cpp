#include "markdown_edit_view.h"

#include "key_binding.h"
#include "list_line.h"
#include "note_view.h"          // applyPalette — палитра у всех видов одна
#include "syntax_highlighter.h"

#include <QAbstractTextDocumentLayout>

#include <QColor>
#include <QFontMetricsF>
#include <QFocusEvent>
#include <QKeyEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QScrollBar>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextLayout>
#include <QWidget>

#include <algorithm>
#include <utility>
#include <vector>

namespace zametti {

// Поле слева от текста: точки у перенесённых строк. Живёт в отступе вьюпорта
// (setViewportMargins) — там, где у редакторов кода стоят номера строк, — и
// перерисовывается по updateRequest вида: прокрутка, правка, смена размера.
class MarkdownEditView::WrapMarks : public QWidget {
public:
    explicit WrapMarks(MarkdownEditView* view) : QWidget(view), view_(view) {}

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        view_->paintWrapMarks(painter, rect());
    }

private:
    MarkdownEditView* view_;
};

MarkdownEditView::MarkdownEditView(QWidget* parent) : QPlainTextEdit(parent) {
    setFrameStyle(QFrame::NoFrame);
    // ПЕРЕНОС ПО ШИРИНЕ ОКНА. Длинную строку markdown (абзац, ссылка, строка
    // таблицы) человек обязан видеть целиком: горизонтальная полоса в
    // текстовом редакторе — это чтение в замочную скважину.
    setLineWrapMode(QPlainTextEdit::WidgetWidth);
    setTabChangesFocus(false);
    // ПОДСВЕТКИ ПЕРЕСЧИТЫВАЮТСЯ ПО ВИДИМОМУ, а не по всей заметке: плашка под
    // каждым блоком кода мегабайтной заметки — это тысячи выделений, которые Qt
    // перебирает на каждый кадр. Поводов пересчитать три: прокрутка, правка и
    // изменение размера (последнее — в resizeEvent).
    //
    // ПРОКРУТКА — ПО ПОЛОСЕ, А НЕ ПО updateRequest: сигнал обновления приходит и
    // от самой setExtraSelections, и получилась бы бесконечная петля.
    connect(verticalScrollBar(), &QScrollBar::valueChanged, this, [this] { refreshOverlays(); });
    connect(document(), &QTextDocument::contentsChanged, this, [this] { refreshOverlays(); });

    // Поле с точками перенесённых строк — перерисовывается вслед за видом.
    wrapMarks_ = new WrapMarks(this);
    connect(this, &QPlainTextEdit::updateRequest, this, [this](const QRect&, int) { wrapMarks_->update(); });

    // Штатную каретку гасим: рисуем свою (см. paintEvent). Пока человек
    // печатает или ведёт курсор, она горит ровно.
    setCursorWidth(0);
    connect(&caretBlink_, &CaretBlink::phaseChanged, this,
            [this] { viewport()->update(caretRect()); });
    connect(this, &QPlainTextEdit::cursorPositionChanged, this, &MarkdownEditView::showCaret);
    connect(this, &QPlainTextEdit::textChanged, this, &MarkdownEditView::showCaret);
    refreshAppearance();
}

// --- каретка ------------------------------------------------------------------

QRect MarkdownEditView::caretRect() const {
    QRect at = cursorRect();
    at.setWidth(caretPixelWidth(settings().style().caretWidth(), zoom_));
    // С запасом: перерисовываем чуть больше, чем красим, иначе остаётся след.
    return at.adjusted(-2, -2, 4, 2);
}

void MarkdownEditView::showCaret() {
    caretBlink_.wake(hasFocus() && !isReadOnly());
    // Целиком: курсор мог только что уехать, и на прежнем месте осталась бы
    // нарисованная каретка.
    viewport()->update();
}

void MarkdownEditView::focusInEvent(QFocusEvent* event) {
    QPlainTextEdit::focusInEvent(event);
    showCaret();
}

void MarkdownEditView::focusOutEvent(QFocusEvent* event) {
    QPlainTextEdit::focusOutEvent(event);
    caretBlink_.sleep();
    viewport()->update();
}

// Колонка каретки, перерисованная без штатного курсора — ровно тот же ход,
// что у NoteView::repaintOverNativeCaret (коммит f48623d): штатную каретку
// будят клавиши, мышь и набор, погасить её насовсем Qt не даёт (ширина 0 на
// дробном масштабе экрана становится физическим пикселем, и рядом с нашей
// мигает чужая чёрная черта). Поэтому после штатной отрисовки колонка
// рисуется заново — фон, плашка кода, подсветка поиска, текст, — и чужая
// каретка не переживает ни одного кадра. У QPlainTextEdit documentLayout()
// не рисует (ленивая вёрстка), блоки он рисует сам: повторяем это для одного
// блока — блока каретки — через его QTextLayout.
void MarkdownEditView::repaintOverNativeCaret(QPainter& painter) {
    if (isReadOnly()) return;
    const QRect cursor = cursorRect();
    const QRect col(cursor.left() - 3, cursor.top() - 2, 10, cursor.height() + 4);
    const QTextBlock block = textCursor().block();
    if (!block.isValid() || block.layout() == nullptr) return;

    painter.save();
    painter.setClipRect(col);
    painter.fillRect(col, palette().color(QPalette::Base));

    // Подсветки этого блока — как их собрал бы сам Qt: плашка во всю ширину
    // кладётся фоном, найденное — отрезками формата.
    const int from = block.position();
    const int to = from + block.length();
    QList<QTextLayout::FormatRange> ranges;
    for (const QTextEdit::ExtraSelection& one : extraSelections()) {
        const int a = qMin(one.cursor.anchor(), one.cursor.position());
        const int b = qMax(one.cursor.anchor(), one.cursor.position());
        if (one.format.boolProperty(QTextFormat::FullWidthSelection)) {
            if (one.cursor.position() >= from && one.cursor.position() < to)
                painter.fillRect(col, one.format.background());
            continue;
        }
        if (b <= from || a >= to) continue;
        QTextLayout::FormatRange range;
        range.start = qMax(a, from) - from;
        range.length = qMin(b, to) - qMax(a, from);
        range.format = one.format;
        ranges.push_back(range);
    }
    if (textCursor().hasSelection()) {
        const int a = qMin(textCursor().anchor(), textCursor().position());
        const int b = qMax(textCursor().anchor(), textCursor().position());
        if (b > from && a < to) {
            QTextLayout::FormatRange range;
            range.start = qMax(a, from) - from;
            range.length = qMin(b, to) - qMax(a, from);
            range.format.setBackground(palette().brush(QPalette::Highlight));
            range.format.setForeground(palette().brush(QPalette::HighlightedText));
            ranges.push_back(range);
        }
    }
    const QPointF offset = blockBoundingGeometry(block).translated(contentOffset()).topLeft();
    painter.setPen(palette().color(QPalette::Text));
    block.layout()->draw(&painter, offset, ranges, col);
    painter.restore();
}

void MarkdownEditView::paintEvent(QPaintEvent* event) {
    QPlainTextEdit::paintEvent(event);
    QPainter painter(viewport());
    repaintOverNativeCaret(painter);
    // Каретка — последней, поверх текста и плашек; при выделении не рисуется
    // (там видно и так), без фокуса — тоже. Правило то же, что у NoteView.
    if (!caretBlink_.on() ||
        !caretShouldBeDrawn(hasFocus(), isReadOnly(), textCursor().hasSelection(), false))
        return;
    QRect at = cursorRect();
    at.setWidth(caretPixelWidth(settings().style().caretWidth(), zoom_));
    painter.fillRect(at, settings().style().caretColor());
}

MarkdownEditView::~MarkdownEditView() {
    // ПОРЯДОК РАЗРУШЕНИЯ. Члены умирают в обратном порядке объявления, а
    // сигналы живут до ~QObject — то есть дольше членов. Подсветчик при смерти
    // отвязывается от документа, документ испускает textChanged, слот showCaret
    // ещё подключён — и будит уже разрушенный таймер мигания (так и нашлось:
    // bad_alloc из QObject::startTimer в ~Rig набора). Снимаем свои соединения,
    // пока всё живо, и отпускаем подсветчик сами.
    disconnect(this, nullptr, this, nullptr);
    disconnect(document(), nullptr, this, nullptr);
    disconnect(verticalScrollBar(), nullptr, this, nullptr);
    disconnect(&caretBlink_, nullptr, this, nullptr);
    highlighter_.reset();
}

void MarkdownEditView::refreshAppearance() {
    applyPalette(*this, /*history=*/false, settings().style());

    // ПОДСВЕТКА — тот же класс ядра, что расцвечивает строки разности
    // (ZSyntaxHighlighterMD). Он и писался под этот режим. Прикрепляется к
    // СВОЕМУ документу: заметка тут ни при чём.
    highlighter_ = std::make_shared<ZSyntaxHighlighterMD>(
        document(), settings().markdownHighlighting(), 0);
    highlighter_->rehighlight();

    // Сочетание переключения задачи — из настроек, списком, тем же разборщиком,
    // что у обычного вида (key_binding.h); разбирается здесь, а не на каждое
    // нажатие.
    toggleTaskKeys_ = keySequencesOf(settings().editor().toggleTaskKey());

    applyZoom(zoom_);   // шрифт, стоп табуляции и поля — одним местом
}

void MarkdownEditView::applyZoom(qreal zoom) {
    const ZDocStyle& style = settings().style();
    zoom_ = qBound(settings().ui().zoomMin(), zoom, settings().ui().zoomMax());

    // ГАРНИТУРА КОДА: исходник читают как код — по колонкам, и пропорциональный
    // шрифт сбил бы и таблицы, и отступы списков.
    QFont font(style.codeFamily());
    font.setPointSizeF(style.baseFontPoint() * zoom_);
    setFont(font);
    document()->setDefaultFont(font);
    // Стоп табуляции — тот же, которым Tab ставит пробелы: набранное и старые
    // литеральные табы обязаны рисоваться одинаково. Считается от НЫНЕШНЕГО
    // шрифта: с масштабом стоп обязан расти вместе с буквами.
    const int stop = qMax(1, settings().editor().codeTabWidth());
    setTabStopDistance(stop * QFontMetricsF(font).horizontalAdvance(QLatin1Char(' ')));

    applyContentWidth();
    refreshOverlays();
}

void MarkdownEditView::applyContentWidth() {
    // ПОЛЯ ВЬЮПОРТА, как в обычном виде: на широком экране длинная строка не
    // читается — глаз теряет начало следующей. Колонка ограничена той же
    // настройкой (maxContentWidth в ширинах буквы «A») и теми же боковыми
    // полями, поэтому исходник и вёрстка стоят на одном месте.
    const QFontMetricsF metrics(font());
    const qreal charUnit = metrics.horizontalAdvance(QLatin1Char('A'));
    const ZDocStyle& style = settings().style();
    const qreal side = style.sideMargin() * charUnit;

    // Полная ширина, из которой раздаётся место: нынешний вьюпорт плюс то, что
    // мы у него уже отняли. По width() виджета считать нельзя — там ещё полоса
    // прокрутки, и вышла бы обратная связь.
    const int room = viewport()->width() + viewportMargin_ * 2;
    qreal margin = side;
    if (style.maxContentWidth() > 0.0) {
        const qreal limit = style.maxContentWidth() * charUnit;
        const qreal spare = (room - 2 * side - limit) / 2;
        if (spare > 0.0) margin += spare;
    }
    const int wanted = qMax(0, int(margin));
    if (wanted == viewportMargin_) return;
    viewportMargin_ = wanted;
    setViewportMargins(wanted, 0, wanted, 0);
    placeWrapMarks();
}

void MarkdownEditView::resizeEvent(QResizeEvent* event) {
    QPlainTextEdit::resizeEvent(event);
    applyContentWidth();
    placeWrapMarks();
    refreshOverlays();
}

// --- точки у перенесённых строк -----------------------------------------------

void MarkdownEditView::placeWrapMarks() {
    if (wrapMarks_ == nullptr) return;
    const QRect vp = viewport()->geometry();
    wrapMarks_->setGeometry(vp.left() - viewportMargin_, vp.top(), viewportMargin_, vp.height());
    wrapMarks_->update();
}

void MarkdownEditView::paintWrapMarks(QPainter& painter, const QRect& area) {
    // Фон — страница, как под текстом: поле не должно читаться рамкой.
    painter.fillRect(area, palette().color(QPalette::Base));

    // Точка — на половине высоты строчной буквы (как «·»), диаметром от кегля,
    // не тоньше двух пикселей; цветом комментариев подсветки — она служебная,
    // а не текст. Стоит на один пробел левее текста.
    const QFontMetricsF metrics(font());
    const qreal xHeight = metrics.xHeight();
    const qreal d = qMax(2.0, xHeight * 0.3);
    const qreal x = area.right() - metrics.horizontalAdvance(QLatin1Char(' ')) - d;
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(Qt::NoPen);
    painter.setBrush(settings().markdownHighlighting().comment());

    const int bottom = area.bottom();
    for (QTextBlock block = firstVisibleBlock(); block.isValid(); block = block.next()) {
        const QRectF geometry = blockBoundingGeometry(block).translated(contentOffset());
        if (geometry.top() > bottom) break;
        const QTextLayout* layout = block.layout();
        if (layout == nullptr) continue;
        for (int i = 1; i < layout->lineCount(); ++i) {
            const QTextLine line = layout->lineAt(i);
            const qreal y = geometry.top() + line.y() + line.ascent() - xHeight / 2;
            painter.drawEllipse(QRectF(x, y - d / 2, d, d));
        }
    }
}

void MarkdownEditView::showSource(const QString& markdown, SourcePos caret) {
    clearMatches();
    setPlainText(markdown);
    // СВОЯ ИСТОРИЯ ПРАВКИ НАЧИНАЕТСЯ ЗАНОВО: подстановка текста — не правка
    // человека, и отменять её нечего (иначе первый же Ctrl+Z опустошил бы вид).
    document()->clearUndoRedoStacks();
    document()->setModified(false);

    QTextCursor at(document());
    const QTextBlock line = document()->findBlockByNumber(qMax(0, caret.line));
    if (line.isValid())
        at.setPosition(line.position() + qBound(0, caret.column, line.length() - 1));
    setTextCursor(at);
    centerCursor();
}

SourcePos MarkdownEditView::caretPos() const {
    SourcePos pos;
    const QTextCursor at = textCursor();
    pos.line = at.blockNumber();
    pos.column = at.position() - at.block().position();
    return pos;
}

// --- клавиши ---------------------------------------------------------------
//
// ПРАВИЛА — НАД ТЕКСТОМ, и только над ним: заметка в режиме правку не
// принимает, истина живёт в тексте вида. Что такое «строка пункта», решает
// parseListLine (list_line.h) — то же правило, которым подсветчик красит
// маркер. Всё, что надо знать о строках, узнаётся ДО скобки отмены: состояния
// блоков (в заборе ли) подсветчик перечитывает только на внешнем endEditBlock,
// и внутри скобки они устарели. Каждое нажатие — одна скобка, один шаг отмены.

namespace {

// Строки выделения целиком: [первая, последняя]. Выделения нет — строка каретки.
struct LineSpan {
    int first = 0;
    int last = 0;
};

LineSpan spanOf(const QTextCursor& at) {
    const QTextDocument* doc = at.document();
    const int from = qMin(at.anchor(), at.position());
    const int to = qMax(at.anchor(), at.position());
    LineSpan span;
    span.first = doc->findBlock(from).blockNumber();
    span.last = doc->findBlock(to).blockNumber();
    // Выделение, кончающееся ровно в начале строки, эту строку НЕ ЗАХВАТЫВАЕТ:
    // человек вёл его до конца предыдущей.
    if (span.last > span.first && doc->findBlock(to).position() == to) --span.last;
    return span;
}

// Строка внутри забора кода — и сам забор, открывающий и закрывающий. Тот же
// признак, которым кладётся плашка.
bool isCodeLine(const QTextBlock& block) {
    return ZSyntaxHighlighterMD::inFence(block.userState()) ||
           (block.previous().isValid() && ZSyntaxHighlighterMD::inFence(block.previous().userState()));
}

// Пункт ли эта строка. Внутри кода пунктов не бывает, как бы строка ни
// выглядела: «- a» в заборе — это код.
ListLine itemOf(const QTextBlock& block, int stop) {
    if (!block.isValid() || isCodeLine(block)) return {};
    return parseListLine(block.text(), stop);
}

int indentColumnOf(const QTextBlock& block, int stop) {
    const QString text = block.text();
    return columnOf(text, leadingWhitespace(text), stop);
}

// Отступ строки знаками — дословно, табы вместе с пробелами.
QString indentStringOf(const QTextBlock& block) {
    const QString text = block.text();
    return text.left(leadingWhitespace(text));
}

// Отступ, которым пункт встаёт ПОД этот пункт: его отступ дословно плюс пробелы
// до его колонки содержимого. Дословно — чтобы чужие табы остались табами.
QString childIndentOf(const QTextBlock& parent, const ListLine& item) {
    return indentStringOf(parent) + QString(qMax(0, item.contentColumn - item.indent), QLatin1Char(' '));
}

// Предыдущий пункт ТОГО ЖЕ отступа — сосед, под которого пункт уходит по Tab.
// Пустые строки и более глубокие пункты пропускаются; более мелкий пункт
// (мы — первый ребёнок) или чужой текст не глубже нашего (список кончился) —
// соседа нет.
QTextBlock siblingAbove(const QTextBlock& block, const ListLine& mine, int stop) {
    for (QTextBlock b = block.previous(); b.isValid(); b = b.previous()) {
        if (isBlankLine(b.text())) continue;
        const ListLine other = itemOf(b, stop);
        if (other.item) {
            if (other.indent == mine.indent) return b;
            if (other.indent < mine.indent) return {};
            continue;
        }
        if (indentColumnOf(b, stop) <= mine.indent) return {};
    }
    return {};
}

// Ближайший пункт выше с МЕНЬШИМ отступом — родитель; на его отступ пункт
// выходит по Shift+Tab. Нет — выходит на нулевой.
QTextBlock parentAbove(const QTextBlock& block, const ListLine& mine, int stop) {
    for (QTextBlock b = block.previous(); b.isValid(); b = b.previous()) {
        if (isBlankLine(b.text())) continue;
        const ListLine other = itemOf(b, stop);
        if (other.item && other.indent < mine.indent) return b;
    }
    return {};
}

// Пункт, который продолжает Shift+Enter: ближайший пункт на строке каретки или
// выше, не глубже неё. Чужой текст мельче строки по дороге — список кончился,
// продолжать нечего. Пустая строка каретки смотрит на предыдущую непустую.
struct Continuation {
    bool found = false;
    QString indent;   // отступ новой строки — до колонки содержимого пункта
};

Continuation continuationOf(const QTextBlock& block, int stop) {
    QTextBlock ref = block;
    while (ref.isValid() && isBlankLine(ref.text())) ref = ref.previous();
    if (!ref.isValid()) return {};
    const int refIndent = indentColumnOf(ref, stop);
    for (QTextBlock b = ref; b.isValid(); b = b.previous()) {
        if (isBlankLine(b.text())) continue;
        const ListLine other = itemOf(b, stop);
        if (other.item) {
            if (other.indent > refIndent) continue;
            // Продолжать можно сам пункт или то, что лежит в его содержимом.
            if (b != ref && refIndent < other.contentColumn) return {};
            return {true, childIndentOf(b, other)};
        }
        if (indentColumnOf(b, stop) < refIndent) return {};
    }
    return {};
}

}  // namespace

void MarkdownEditView::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape) {
        emit leaveRequested();
        event->accept();
        return;
    }

    // ДНО СТЕКА ОТМЕНЫ. В обычном виде Ctrl+Z на дне ведёт в слепки журнала;
    // у режима стек свой, и на его дне отмена отдаётся заметке — иначе режим
    // был бы тупиком, где Ctrl+Z молча ничего не делает.
    if (event->matches(QKeySequence::Undo) && !document()->isUndoAvailable()) {
        emit undoExhausted();
        event->accept();
        return;
    }

    const Qt::KeyboardModifiers mods = event->modifiers() & ~Qt::KeypadModifier;
    const bool enter = event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter;
    if (enter && (mods == Qt::NoModifier || mods == Qt::ShiftModifier)) {
        pressEnter(mods == Qt::ShiftModifier);
        event->accept();
        return;
    }

    const bool tab = event->key() == Qt::Key_Tab && event->modifiers() == Qt::NoModifier;
    const bool backtab = event->key() == Qt::Key_Backtab ||
                         (event->key() == Qt::Key_Tab && event->modifiers() == Qt::ShiftModifier);
    if (tab || backtab) {
        pressTab(backtab);
        event->accept();   // наружу Tab не отдаём: Shift+Tab увёл бы фокус
        return;
    }

    if (keyEventMatchesAny(*event, toggleTaskKeys_)) {
        toggleTasks();
        event->accept();
        return;
    }

    QPlainTextEdit::keyPressEvent(event);
}

// ENTER: пункт продолжается пунктом (пустой пункт — выходит из списка), всё
// остальное — переносом с отступом строки. SHIFT+ENTER: продолжение пункта —
// новая строка под первым знаком содержимого; вне списка — как Enter.
void MarkdownEditView::pressEnter(bool shift) {
    const int stop = qMax(1, settings().editor().codeTabWidth());
    QTextCursor at = textCursor();
    const int from = qMin(at.anchor(), at.position());
    const int to = qMax(at.anchor(), at.position());
    const QTextBlock block = document()->findBlock(from);
    const QTextBlock endBlock = document()->findBlock(to);
    const int column = from - block.position();
    // Строка, какой она станет после удаления выделения: голова до каретки и
    // хвост за концом выделения. Правила смотрят на неё, а не на нынешнюю.
    const QString line = block.text().left(column) + endBlock.text().mid(to - endBlock.position());
    const bool code = isCodeLine(block);
    const ListLine item = code ? ListLine{} : parseListLine(line, stop);

    QString insert;
    bool clearLine = false;
    if (!shift && item.item && column >= item.contentStart) {
        if (item.emptyBody)
            clearLine = true;   // пустой пункт + Enter — из списка вон
        else
            insert = QLatin1Char('\n') + line.left(item.indentChars) + nextMarker(item);
    } else {
        Continuation cont;
        // Shift+Enter продолжает пункт, если каретка стоит в его содержимом (не
        // в отступе и не в маркере) и строка не код.
        if (shift && !code && column >= leadingWhitespace(line) &&
            (!item.item || column >= item.contentStart))
            cont = continuationOf(block, stop);
        if (cont.found) {
            insert = QLatin1Char('\n') + cont.indent;
        } else {
            // Автоотступ: отступ строки, обрезанный по каретке — в колонке 0
            // строка просто уезжает вниз.
            const int lead = leadingWhitespace(line);
            insert = QLatin1Char('\n') + line.left(qMin(lead, column));
        }
    }

    at.beginEditBlock();
    if (at.hasSelection()) at.removeSelectedText();
    if (clearLine) {
        at.movePosition(QTextCursor::StartOfBlock);
        at.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
        at.removeSelectedText();
    } else {
        at.insertText(insert);
    }
    at.endEditBlock();
    setTextCursor(at);
    ensureCursorVisible();
}

// TAB / SHIFT+TAB. На строке пункта (без выделения в несколько строк) — сам
// пункт: под предыдущего соседа того же отступа / на отступ родителя. В коде и
// вне списков — пробелы до стопа в месте каретки / снять до стопа. Выделение в
// несколько строк — единый сдвиг, дельту задаёт ПЕРВАЯ строка (как
// indentListItems решает по первому блоку): пункт — по своему правилу, иначе
// стоп; пустые строки не трогаются, табы чужого отступа не трогаются.
void MarkdownEditView::pressTab(bool back) {
    const int stop = qMax(1, settings().editor().codeTabWidth());
    QTextCursor at = textCursor();
    const LineSpan span = spanOf(at);
    const bool manyLines = span.last > span.first;
    const QTextBlock first = document()->findBlockByNumber(span.first);
    const ListLine item = itemOf(first, stop);

    // Новый отступ первой строки-пункта по правилу списка; пусто в newIndent при
    // !listMove — правило не применимо (не пункт) или отказало.
    bool listMove = false;
    bool refused = false;
    QString newIndent;
    if (item.item) {
        listMove = true;
        if (!back) {
            const QTextBlock sibling = siblingAbove(first, item, stop);
            if (!sibling.isValid()) refused = true;
            else newIndent = childIndentOf(sibling, itemOf(sibling, stop));
        } else {
            if (item.indent == 0) refused = true;
            else {
                const QTextBlock parent = parentAbove(first, item, stop);
                newIndent = parent.isValid() ? indentStringOf(parent) : QString();
            }
        }
    }
    if (listMove && refused) return;

    // ОДНА СТРОКА, НЕ ПУНКТ: Tab — отступ до стопа в месте каретки (набранное
    // встаёт в колонку), Shift+Tab — снять до стопа ведущих пробелов.
    if (!manyLines && !listMove) {
        if (!back) {
            if (at.hasSelection()) at.removeSelectedText();
            const int column = at.position() - at.block().position();
            at.insertText(QString(stop - column % stop, QLatin1Char(' ')));
            setTextCursor(at);
            return;
        }
        const QTextBlock line = at.block();
        const QString text = line.text();
        int drop = 0;
        while (drop < stop && drop < text.size() && text.at(drop) == QLatin1Char(' ')) ++drop;
        if (drop == 0) return;
        QTextCursor edit(line);
        edit.setPosition(line.position() + drop, QTextCursor::KeepAnchor);
        edit.removeSelectedText();
        return;
    }

    // Дельта в знаках для остальных строк выделения.
    const int delta = listMove ? int(newIndent.size()) - item.indentChars : (back ? -stop : stop);
    const int anchor = at.anchor();
    const int position = at.position();
    const int caretColumn = position - at.block().position();
    const int caretBlock = at.blockNumber();

    at.beginEditBlock();
    for (int number = span.first; number <= span.last; ++number) {
        const QTextBlock line = document()->findBlockByNumber(number);
        if (!line.isValid()) continue;
        const QString text = line.text();
        if (manyLines && isBlankLine(text)) continue;
        QTextCursor edit(line);
        if (number == span.first && listMove) {
            // Первая строка-пункт: её отступ заменяется целиком на новый.
            edit.setPosition(line.position() + item.indentChars, QTextCursor::KeepAnchor);
            edit.insertText(newIndent);
            continue;
        }
        if (delta > 0) {
            edit.insertText(QString(delta, QLatin1Char(' ')));
            continue;
        }
        int drop = 0;
        while (drop < -delta && drop < text.size() && text.at(drop) == QLatin1Char(' ')) ++drop;
        if (drop == 0) continue;
        edit.setPosition(line.position() + drop, QTextCursor::KeepAnchor);
        edit.removeSelectedText();
    }
    at.endEditBlock();

    if (!manyLines) {
        // Каретка остаётся на своём знаке строки; если стояла в отступе — на
        // его конце.
        const QTextBlock line = document()->findBlockByNumber(caretBlock);
        const int shifted = qMax(int(newIndent.size()), caretColumn + delta);
        QTextCursor moved(document());
        moved.setPosition(line.position() + qMin(shifted, qMax(0, line.length() - 1)));
        setTextCursor(moved);
        return;
    }
    // Выделение в несколько строк после сдвига охватывает те же строки целиком:
    // концы, которые Qt сдвинул вслед за правкой, человеку ни о чём не говорят,
    // а строки — говорят: второй Tab двигает их же.
    const QTextBlock firstLine = document()->findBlockByNumber(span.first);
    const QTextBlock lastLine = document()->findBlockByNumber(span.last);
    const int head = firstLine.position();
    const int tail = lastLine.position() + qMax(0, lastLine.length() - 1);
    QTextCursor whole(document());
    whole.setPosition(anchor <= position ? head : tail);
    whole.setPosition(anchor <= position ? tail : head, QTextCursor::KeepAnchor);
    setTextCursor(whole);
}

// ПЕРЕКЛЮЧЕНИЕ ЗАДАЧИ (toggleTaskKey): строки выделения или строка каретки;
// первая задача задаёт направление, остальные идут за ней; задач нет — ничего
// (как toggleTask в обычном виде). Длина строк не меняется — выделение цело.
void MarkdownEditView::toggleTasks() {
    const int stop = qMax(1, settings().editor().codeTabWidth());
    QTextCursor at = textCursor();
    const LineSpan span = spanOf(at);

    bool found = false;
    bool target = true;
    std::vector<std::pair<int, bool>> tasks;   // позиция знака в скобках, нынешнее
    for (int number = span.first; number <= span.last; ++number) {
        const QTextBlock line = document()->findBlockByNumber(number);
        const ListLine item = itemOf(line, stop);
        if (!item.item || item.marker != Marker::Task) continue;
        if (!found) {
            found = true;
            target = !item.checked;
        }
        tasks.push_back({line.position() + item.markerEnd - 2, item.checked});
    }
    if (!found) return;

    const int anchor = at.anchor();
    const int position = at.position();
    QTextCursor edit(document());
    edit.beginEditBlock();
    for (const auto& [pos, checked] : tasks) {
        if (checked == target) continue;
        edit.setPosition(pos);
        edit.setPosition(pos + 1, QTextCursor::KeepAnchor);
        edit.insertText(target ? QStringLiteral("x") : QStringLiteral(" "));
    }
    edit.endEditBlock();
    // Замена знака на знак длину не меняет — концы выделения возвращаем как были.
    QTextCursor same(document());
    same.setPosition(anchor);
    same.setPosition(position, QTextCursor::KeepAnchor);
    setTextCursor(same);
}

// --- поиск ------------------------------------------------------------------

int MarkdownEditView::findMatches(const QString& text, bool caseSensitive) {
    matches_.clear();
    current_ = -1;
    needle_ = int(text.size());
    if (text.isEmpty()) {
        refreshOverlays();
        return 0;
    }
    const QString hay = toPlainText();
    const Qt::CaseSensitivity how = caseSensitive ? Qt::CaseSensitive : Qt::CaseInsensitive;
    for (int at = hay.indexOf(text, 0, how); at >= 0; at = hay.indexOf(text, at + 1, how))
        matches_.push_back(at);
    // Ближайшее вперёд от каретки — чтобы первый F3 шёл оттуда, где человек
    // стоит, а не с начала заметки.
    const int caret = textCursor().position();
    for (size_t i = 0; i < matches_.size(); ++i)
        if (matches_[i] >= caret) {
            current_ = int(i);
            break;
        }
    if (current_ < 0 && !matches_.empty()) current_ = 0;
    refreshOverlays();
    return int(matches_.size());
}

bool MarkdownEditView::stepMatch(int direction) {
    if (matches_.empty()) return false;
    const int count = int(matches_.size());
    current_ = current_ < 0 ? 0 : (current_ + (direction >= 0 ? 1 : count - 1)) % count;
    QTextCursor at(document());
    at.setPosition(matches_[size_t(current_)]);
    at.setPosition(matches_[size_t(current_)] + needle_, QTextCursor::KeepAnchor);
    setTextCursor(at);
    centerCursor();
    refreshOverlays();
    return true;
}

void MarkdownEditView::clearMatches() {
    matches_.clear();
    current_ = -1;
    needle_ = 0;
    setExtraSelections({});
}

void MarkdownEditView::refreshOverlays() {
    QList<QTextEdit::ExtraSelection> shown;

    // 1. ПЛАШКА ПОД БЛОКАМИ КОДА (просьба владельца: код видно и в исходнике).
    //
    // Выделением во всю ширину, а не своей отрисовкой: QPlainTextEdit заливает
    // вьюпорт фоном САМ, перед текстом, и нарисованное до него стёрлось бы, а
    // нарисованное после — легло бы поверх букв.
    //
    // Строка внутри забора несёт состояние «в заборе» (его ставит подсветчик);
    // у закрывающего забора состояние уже Plain, но его предшественница — в
    // заборе. Тот же приём, что у вида разности.
    const QColor plate = settings().markdownHighlighting().codeBackground();
    const int height = viewport()->height();
    for (QTextBlock block = firstVisibleBlock(); block.isValid(); block = block.next()) {
        if (blockBoundingGeometry(block).translated(contentOffset()).top() > height) break;
        const bool inFence =
            ZSyntaxHighlighterMD::inFence(block.userState()) ||
            (block.previous().isValid() &&
             ZSyntaxHighlighterMD::inFence(block.previous().userState()));
        if (!inFence) continue;
        // ПОЛОСА НА КАЖДУЮ ВИЗУАЛЬНУЮ СТРОКУ, а не на блок: выделение без
        // диапазона с FullWidthSelection Qt красит ровно ту визуальную строку,
        // где стоит позиция курсора. Длинная строка кода в узком окне
        // переносится — и её хвосты шли на подложке обычного текста (нашёл
        // владелец). Видимые блоки у QPlainTextEdit свёрстаны всегда.
        const QTextLayout* layout = block.layout();
        const int lines = layout != nullptr ? qMax(1, layout->lineCount()) : 1;
        for (int i = 0; i < lines; ++i) {
            QTextEdit::ExtraSelection band;
            band.cursor = QTextCursor(block);
            if (layout != nullptr && i < layout->lineCount())
                band.cursor.setPosition(block.position() + layout->lineAt(i).textStart());
            band.format.setBackground(plate);
            band.format.setProperty(QTextFormat::FullWidthSelection, true);
            shown.push_back(band);
        }
    }

    // 2. НАЙДЕННОЕ — поверх плашки, тоже только видимое.
    if (needle_ > 0) {
        const int from = cursorForPosition(QPoint(0, 0)).position();
        const int to = cursorForPosition(QPoint(viewport()->width(), viewport()->height()))
                           .position() + needle_;
        for (size_t i = 0; i < matches_.size(); ++i) {
            const int at = matches_[i];
            if (at + needle_ < from || at > to) continue;
            QTextEdit::ExtraSelection one;
            one.cursor = QTextCursor(document());
            one.cursor.setPosition(at);
            one.cursor.setPosition(at + needle_, QTextCursor::KeepAnchor);
            // Цвет один на всю программу (searchHighlight); текущее совпадение
            // не другим цветом, а заметнее — ровно так же, как в NoteView.
            QColor tint = settings().style().searchHighlight();
            if (int(i) != current_) tint.setAlpha(110);
            one.format.setBackground(tint);
            shown.push_back(one);
        }
    }
    setExtraSelections(shown);
}

bool MarkdownEditView::replaceCurrent(const QString& with) {
    if (current_ < 0 || size_t(current_) >= matches_.size()) return false;
    QTextCursor at(document());
    at.setPosition(matches_[size_t(current_)]);
    at.setPosition(matches_[size_t(current_)] + needle_, QTextCursor::KeepAnchor);
    at.insertText(with);
    setTextCursor(at);
    return true;
}

int MarkdownEditView::replaceAll(const QString& text, bool caseSensitive, const QString& with) {
    if (text.isEmpty()) return 0;
    const QString hay = toPlainText();
    const Qt::CaseSensitivity how = caseSensitive ? Qt::CaseSensitive : Qt::CaseInsensitive;
    std::vector<int> at;
    for (int i = hay.indexOf(text, 0, how); i >= 0; i = hay.indexOf(text, i + 1, how))
        at.push_back(i);
    if (at.empty()) return 0;
    // ОДНА СКОБКА НА ВСЁ: иначе откатывать пришлось бы по одному вхождению.
    // Идём с конца — передние замены не сдвигают ещё не сделанные.
    QTextCursor edit(document());
    edit.beginEditBlock();
    for (auto i = at.rbegin(); i != at.rend(); ++i) {
        QTextCursor one(document());
        one.setPosition(*i);
        one.setPosition(*i + int(text.size()), QTextCursor::KeepAnchor);
        one.insertText(with);
    }
    edit.endEditBlock();
    clearMatches();
    return int(at.size());
}

}  // namespace zametti
