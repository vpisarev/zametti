#include "plain_edit_view.h"

#include "content_column.h"

#include <cstdio>

#include "note_view.h"   // applyPalette, caretShouldBeDrawn — правила у всех видов одни
#include "settings.h"
#include "zoom_scale.h"

#include <QAbstractTextDocumentLayout>
#include <QColor>
#include <QFocusEvent>
#include <QFontMetricsF>
#include <QKeyEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QScrollBar>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextLayout>
#include <QWheelEvent>
#include <QWidget>

#include <algorithm>

namespace zametti {

// Поле слева от текста: точки у перенесённых строк. Живёт в отступе вьюпорта
// (setViewportMargins) — там, где у редакторов кода стоят номера строк, — и
// перерисовывается по updateRequest вида: прокрутка, правка, смена размера.
class PlainEditView::WrapMarks : public QWidget {
public:
    explicit WrapMarks(PlainEditView* view) : QWidget(view), view_(view) {}

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        view_->paintWrapMarks(painter, rect());
    }

private:
    PlainEditView* view_;
};

PlainEditView::PlainEditView(QWidget* parent) : QPlainTextEdit(parent) {
    setFrameStyle(QFrame::NoFrame);
    // ПЕРЕНОС ПО ШИРИНЕ ОКНА. Длинную строку человек обязан видеть целиком:
    // горизонтальная полоса в текстовом редакторе — это чтение в замочную
    // скважину.
    setLineWrapMode(QPlainTextEdit::WidgetWidth);
    setTabChangesFocus(false);
    // ПОДСВЕТКИ ПЕРЕСЧИТЫВАЮТСЯ ПО ВИДИМОМУ, а не по всему тексту: плашка под
    // каждым блоком кода мегабайтной заметки — это тысячи выделений, которые Qt
    // перебирает на каждый кадр. Поводов пересчитать три: прокрутка, правка и
    // изменение размера (последнее — в resizeEvent).
    //
    // ПРОКРУТКА — ПО ПОЛОСЕ, А НЕ ПО updateRequest: сигнал обновления приходит и
    // от самой setExtraSelections, и получилась бы бесконечная петля.
    //
    // И ОБА ЭТИХ ПОВОДА — ЧЕРЕЗ ОЧЕРЕДЬ, а не прямым вызовом. Оба сигнала
    // приходят ИЗНУТРИ чужой работы: полоса двигается посреди
    // QWidgetTextControl::setTextCursor (переход к найденному прокручивает вид
    // сам), а contentsChanged — посреди правки документа. Пересчёт подсветок
    // зовёт setExtraSelections, а тот спрашивает у Qt геометрию выделений —
    // и, попав в середину setTextCursor, читает ещё не достроенное состояние
    // документа. Это не «иногда мигает», а ПАДЕНИЕ: владелец ловил его в
    // режиме [M] шагом поиска по большой заметке (F3 после Ctrl+F), стек —
    // setTextCursor → valueChanged → refreshOverlays → setExtraSelections →
    // QTextCursor::hasComplexSelection → QTextFrame::childFrames → SIGSEGV.
    //
    // Очередь снимает это по построению: пересчёт случится, когда Qt доделает
    // своё и вернётся в цикл событий. Задержки человек не видит — кадр всё
    // равно рисуется после возврата в цикл.
    overlaysSoon_.setSingleShot(true);
    overlaysSoon_.setInterval(0);
    connect(&overlaysSoon_, &QTimer::timeout, this, [this] { refreshOverlays(); });
    connect(verticalScrollBar(), &QScrollBar::valueChanged, this,
            [this] { overlaysSoon_.start(); });
    connect(document(), &QTextDocument::contentsChanged, this, [this] { overlaysSoon_.start(); });
    // ПОСЛЕ ПРАВКИ ПОИСК ПОВТОРЯЕТСЯ ЦЕЛИКОМ (решение владельца: «смещения
    // изменились и количество изменилось»). Находки — числа в тексте, и всякая
    // правка их обесценивает: подсветка оставалась стоять там, где текста уже
    // нет, а счётчик показывал старое число.
    //
    // Пока не пересчитали — подсветки нет вовсе: врать хуже, чем молчать.
    // Сам пересчёт через короткую паузу, а не на каждую букву: он читает весь
    // текст, и на заметке в мегабайт это была бы работа на каждое нажатие.
    connect(document(), &QTextDocument::contentsChange, this,
            [this](int, int removed, int added) {
                if (removed == 0 && added == 0) return;   // правка формата
                if (!query_.usable()) return;
                matches_.clear();
                current_ = -1;
                overlaysSoon_.start();
                searchSoon_.start();
            });
    searchSoon_.setSingleShot(true);
    searchSoon_.setInterval(300);
    connect(&searchSoon_, &QTimer::timeout, this, [this] {
        if (!query_.usable()) return;
        findMatches(query_);
        emit matchesChanged();   // полосе поиска пора показать новое число
    });

    // Поле с точками перенесённых строк — перерисовывается вслед за видом.
    wrapMarks_ = new WrapMarks(this);
    connect(this, &QPlainTextEdit::updateRequest, this, [this](const QRect&, int) { wrapMarks_->update(); });

    // Штатную каретку гасим: рисуем свою (см. paintEvent). Пока человек
    // печатает или ведёт курсор, она горит ровно.
    setCursorWidth(0);
    connect(&caretBlink_, &CaretBlink::phaseChanged, this,
            [this] { viewport()->update(caretRect()); });
    connect(this, &QPlainTextEdit::cursorPositionChanged, this, &PlainEditView::showCaret);
    connect(this, &QPlainTextEdit::textChanged, this, &PlainEditView::showCaret);
}

PlainEditView::~PlainEditView() {
    // ПОРЯДОК РАЗРУШЕНИЯ. Члены умирают в обратном порядке объявления, а
    // сигналы живут до ~QObject — то есть дольше членов. Подсветчик наследника
    // при смерти отвязывается от документа, документ испускает textChanged, слот
    // showCaret ещё подключён — и будит уже разрушенный таймер мигания (так и
    // нашлось: bad_alloc из QObject::startTimer в ~Rig набора). Снимаем свои
    // соединения, пока всё живо.
    disconnect(this, nullptr, this, nullptr);
    disconnect(document(), nullptr, this, nullptr);
    disconnect(verticalScrollBar(), nullptr, this, nullptr);
    disconnect(&caretBlink_, nullptr, this, nullptr);
}

// --- каретка ------------------------------------------------------------------

QRect PlainEditView::caretRect() const {
    QRect at = cursorRect();
    at.setWidth(caretPixelWidth(settings().style().caretWidth(), zoom_));
    // С запасом: перерисовываем чуть больше, чем красим, иначе остаётся след.
    return at.adjusted(-2, -2, 4, 2);
}

void PlainEditView::showCaret() {
    caretBlink_.wake(hasFocus() && !isReadOnly());
    // Целиком: курсор мог только что уехать, и на прежнем месте осталась бы
    // нарисованная каретка.
    viewport()->update();
}

void PlainEditView::focusInEvent(QFocusEvent* event) {
    QPlainTextEdit::focusInEvent(event);
    showCaret();
}

void PlainEditView::focusOutEvent(QFocusEvent* event) {
    QPlainTextEdit::focusOutEvent(event);
    caretBlink_.sleep();
    viewport()->update();
}

// Колонка каретки, перерисованная без штатного курсора — ровно тот же ход,
// что у NoteView::repaintOverNativeCaret (коммит f48623d): штатную каретку
// будят клавиши, мышь и набор, погасить её насовсем Qt не даёт (ширина 0 на
// дробном масштабе экрана становится физическим пикселем, и рядом с нашей
// мигает чужая чёрная черта). Поэтому после штатной отрисовки колонка
// рисуется заново — фон, подсветки, текст, — и чужая каретка не переживает ни
// одного кадра. У QPlainTextEdit documentLayout() не рисует (ленивая вёрстка),
// блоки он рисует сам: повторяем это для одного блока — блока каретки — через
// его QTextLayout.
void PlainEditView::repaintOverNativeCaret(QPainter& painter) {
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

void PlainEditView::paintEvent(QPaintEvent* event) {
    QPlainTextEdit::paintEvent(event);
    QPainter painter(viewport());
    repaintOverNativeCaret(painter);
    // Каретка — последней, поверх текста и плашек; при выделении не рисуется
    // (там видно и так), без фокуса — тоже. Правило то же, что у NoteView.
    if (!caretBlink_.on() ||
        !caretShouldBeDrawn(hasFocus(), isReadOnly(), textCursor().hasSelection(), false))
        return;
    painter.fillRect(caretBar(cursorRect(), settings().style().caretWidth(), zoom_,
                              devicePixelRatioF()),
                     settings().style().caretColor());
}

// --- облик, масштаб, поля -----------------------------------------------------

void PlainEditView::refreshAppearance() {
    applyPalette(*this, /*history=*/false, settings().style());
    applyZoom(zoom_);   // шрифт, стоп табуляции и поля — одним местом
}

int PlainEditView::tabStop() const { return qMax(1, settings().editor().tabWidth()); }

QColor PlainEditView::wrapMarkColor() const { return settings().markdownHighlighting().comment(); }

void PlainEditView::applyZoom(qreal zoom) {
    const ZDocStyle& style = settings().style();
    // Края шкалы — свойство программы, а не настройка (zoom_scale.h).
    zoom_ = qBound(zoomScale(kZoomStepsMin), zoom, zoomScale(kZoomStepsMax));

    // ГАРНИТУРА КОДА: плоский текст читают как код — по колонкам, и
    // пропорциональный шрифт сбил бы и таблицы, и отступы.
    // КЕГЛЬ У ПЛОСКИХ ВИДОВ СВОЙ (fonts.monospaceSize), а не кегль заметки: тут
    // читают колонками, и согласовать моноширинный с основным шрифтом заметки —
    // дело человека, а не наше. Внутри свёрстанной заметки кегль кода остаётся
    // ступенью: там он обязан ехать за одним setDefaultFont.
    QFont font(style.codeFamily());
    font.setPointSizeF(style.monospacePoint() * zoom_);
    setFont(font);
    document()->setDefaultFont(font);
    // Стоп табуляции — тот же, которым Tab ставит пробелы: набранное и старые
    // литеральные табы обязаны рисоваться одинаково. Считается от НЫНЕШНЕГО
    // шрифта: с масштабом стоп обязан расти вместе с буквами.
    setTabStopDistance(tabStop() * QFontMetricsF(font).horizontalAdvance(QLatin1Char(' ')));

    applyContentWidth();
    refreshOverlays();
}

void PlainEditView::applyContentWidth(bool fromResize) {
    // ПОЛЯ ВЬЮПОРТА, как в обычном виде: на широком экране длинная строка не
    // читается — глаз теряет начало следующей. Колонка ограничена той же
    // настройкой (maxContentWidth в ширинах буквы «A») и теми же боковыми
    // полями, поэтому исходник и вёрстка стоят на одном месте.
    // САМ РАСЧЁТ — ОБЩИЙ (content_column.h): тот же, по которому колонку
    // считает вид заметки. Своего бокового поля у плоского вида нет — весь
    // отступ идёт полями вьюпорта, поэтому fromDocument здесь ноль.
    const qreal charUnit = QFontMetricsF(font()).horizontalAdvance(QLatin1Char('A'));
    const int room = viewport()->width() + viewportMargin_ * 2;
    const int wanted = contentColumnMargin(settings().style(), charUnit, room);
    if (wanted == viewportMargin_) return;
    viewportMargin_ = wanted;
    setViewportMargins(wanted, 0, wanted, 0);
    // И ПЕРЕСЧЁТ ВЁРСТКИ — как вид заметки досылает setTextWidth. Поля меняются
    // не только от размера окна: масштаб меняет ширину буквы «A», а по ней
    // считается колонка. Своего setTextWidth у QPlainTextEdit нет — ширину
    // вёрстки он берёт у вьюпорта сам и только в своём resizeEvent; дверь к
    // пересчёту одна — режим переноса, причём ТО ЖЕ значение Qt пропускает,
    // поэтому переключаем через соседнее.
    // Внутри ресайза этого делать НЕЛЬЗЯ и НЕ НУЖНО: базовый обработчик идёт
    // следом и посчитает вёрстку сам, по уже поставленным полям, а
    // переключение режима посреди чужой перекладки — переверстка внутри
    // переверстки.
    if (!fromResize && !rewrapping_) {
        rewrapping_ = true;
        const QPlainTextEdit::LineWrapMode wrap = lineWrapMode();
        setLineWrapMode(wrap == QPlainTextEdit::NoWrap ? QPlainTextEdit::WidgetWidth
                                                       : QPlainTextEdit::NoWrap);
        setLineWrapMode(wrap);
        rewrapping_ = false;
    }
    placeWrapMarks();
}

void PlainEditView::resizeEvent(QResizeEvent* event) {
    // ПОЛЯ — ДО БАЗОВОГО ОБРАБОТЧИКА, и это не вкус, а починка.
    //
    // Ширину переноса QPlainTextEdit считает по своему вьюпорту, и считает её
    // ровно здесь — в QPlainTextEdit::resizeEvent. Поля вьюпорта ставим мы, и
    // пока мы ставили их ПОСЛЕ, документ оставался свёрстан по прежней,
    // БОЛЬШЕЙ ширине: строки переносились позже, чем кончалась видимая область,
    // и хвосты уезжали за правый край. Владелец увидел это, открыв заметку
    // сразу в режиме [M]: «концы длинных строк не отображаются, пропадает
    // несколько слов», а от первого же изменения ширины окна всё чинилось само
    // (второй resize приходил уже с верными полями). Замер в живом окне:
    // вьюпорт 812, а строки свёрстаны по 1017.
    applyContentWidth(/*fromResize=*/true);
    QPlainTextEdit::resizeEvent(event);
    placeWrapMarks();
    refreshOverlays();
}

// --- точки у перенесённых строк -----------------------------------------------

void PlainEditView::placeWrapMarks() {
    if (wrapMarks_ == nullptr) return;
    const QRect vp = viewport()->geometry();
    wrapMarks_->setGeometry(vp.left() - viewportMargin_, vp.top(), viewportMargin_, vp.height());
    wrapMarks_->update();
}

void PlainEditView::paintWrapMarks(QPainter& painter, const QRect& area) {
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
    painter.setBrush(wrapMarkColor());

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

// --- текст --------------------------------------------------------------------

void PlainEditView::setText(const QString& text, int line, int column) {
    clearMatches();
    setPlainText(text);
    // СВОЯ ИСТОРИЯ ПРАВКИ НАЧИНАЕТСЯ ЗАНОВО: подстановка текста — не правка
    // человека, и отменять её нечего (иначе первый же Ctrl+Z опустошил бы вид).
    document()->clearUndoRedoStacks();
    document()->setModified(false);

    QTextCursor at(document());
    const QTextBlock block = document()->findBlockByNumber(qMax(0, line));
    if (block.isValid())
        at.setPosition(block.position() + qBound(0, column, block.length() - 1));
    setTextCursor(at);
    centerCursor();
}

QString PlainEditView::text() const {
    QString out;
    out.reserve(document()->characterCount());
    for (QTextBlock block = document()->begin(); block.isValid(); block = block.next()) {
        if (block.blockNumber() > 0) out += QLatin1Char('\n');
        out += block.text();
    }
    // Разделитель строк внутри блока (приехал из буфера обмена) — это перевод
    // строки; в файле ему делать нечего.
    out.replace(QChar(QChar::LineSeparator), QLatin1Char('\n'));
    return out;
}

int PlainEditView::caretLine() const { return textCursor().blockNumber(); }

int PlainEditView::caretColumn() const {
    const QTextCursor at = textCursor();
    return at.position() - at.block().position();
}

// --- клавиши ------------------------------------------------------------------

PlainEditView::LineSpan PlainEditView::spanOf(const QTextCursor& at) {
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

QString PlainEditView::indentStringOf(const QTextBlock& block) {
    const QString text = block.text();
    int lead = 0;
    while (lead < text.size() && (text.at(lead) == QLatin1Char(' ') || text.at(lead) == QLatin1Char('\t')))
        ++lead;
    return text.left(lead);
}

void PlainEditView::indentLines(int first, int last, int delta) {
    for (int number = first; number <= last; ++number) {
        const QTextBlock line = document()->findBlockByNumber(number);
        if (!line.isValid()) continue;
        const QString text = line.text();
        if (text.trimmed().isEmpty()) continue;
        QTextCursor edit(line);
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
}

void PlainEditView::selectLines(int first, int last, bool forward) {
    const QTextBlock firstLine = document()->findBlockByNumber(first);
    const QTextBlock lastLine = document()->findBlockByNumber(last);
    if (!firstLine.isValid() || !lastLine.isValid()) return;
    const int head = firstLine.position();
    const int tail = lastLine.position() + qMax(0, lastLine.length() - 1);
    QTextCursor whole(document());
    whole.setPosition(forward ? head : tail);
    whole.setPosition(forward ? tail : head, QTextCursor::KeepAnchor);
    setTextCursor(whole);
}

void PlainEditView::wheelEvent(QWheelEvent* event) {
    // Ctrl+колесо — масштаб СТУПЕНЯМИ, тем же правилом, что у NoteView: жест
    // уходит окну, а не правит шрифт на месте — иначе вид разъезжался бы со
    // ступенью в ZAppState.
    if (event->modifiers() & Qt::ControlModifier) {
        const int y = event->angleDelta().y();
        if (y != 0) emit zoomStepRequested(y > 0 ? +1 : -1);
        event->accept();
        return;
    }
    QPlainTextEdit::wheelEvent(event);
}

void PlainEditView::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape) {
        emit leaveRequested();
        event->accept();
        return;
    }

    // Home/End — начало и конец СТРОКИ, то же правило, что у NoteView (на
    // маке умолчание Qt прыгало по документу — решение владельца, 03.09.2026).
    const bool home = event->key() == Qt::Key_Home;
    const bool end = event->key() == Qt::Key_End;
    if ((home || end) &&
        (event->modifiers() & ~(Qt::ShiftModifier | Qt::KeypadModifier)) == 0) {
        QTextCursor cursor = textCursor();
        cursor.movePosition(home ? QTextCursor::StartOfLine : QTextCursor::EndOfLine,
                            event->modifiers() & Qt::ShiftModifier
                                ? QTextCursor::KeepAnchor
                                : QTextCursor::MoveAnchor);
        setTextCursor(cursor);
        ensureCursorVisible();
        event->accept();
        return;
    }

    // ДНО СТЕКА ОТМЕНЫ: отменять в тексте больше нечего — решает контроллер
    // (в исходнике отмена уходит заметке, иначе режим был бы тупиком, где
    // Ctrl+Z молча ничего не делает).
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

    QPlainTextEdit::keyPressEvent(event);
}

void PlainEditView::pressEnter(bool shift) {
    (void)shift;
    QTextCursor at = textCursor();
    const int from = qMin(at.anchor(), at.position());
    const QTextBlock block = document()->findBlock(from);
    const int column = from - block.position();
    // Автоотступ: отступ строки, обрезанный по каретке — в колонке 0 строка
    // просто уезжает вниз.
    const QString indent = indentStringOf(block);
    at.beginEditBlock();
    if (at.hasSelection()) at.removeSelectedText();
    at.insertText(QLatin1Char('\n') + indent.left(qMin(int(indent.size()), column)));
    at.endEditBlock();
    setTextCursor(at);
    ensureCursorVisible();
}

void PlainEditView::pressTab(bool back) {
    const int stop = tabStop();
    QTextCursor at = textCursor();
    const LineSpan span = spanOf(at);
    const bool manyLines = span.last > span.first;

    // ОДНА СТРОКА: Tab — отступ до стопа в месте каретки (набранное встаёт в
    // колонку), Shift+Tab — снять до стопа ведущих пробелов.
    if (!manyLines) {
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

    const bool forward = at.anchor() <= at.position();
    at.beginEditBlock();
    indentLines(span.first, span.last, back ? -stop : stop);
    at.endEditBlock();
    // Выделение в несколько строк после сдвига охватывает те же строки целиком:
    // концы, которые Qt сдвинул вслед за правкой, человеку ни о чём не говорят,
    // а строки — говорят: второй Tab двигает их же.
    selectLines(span.first, span.last, forward);
}

// --- поиск --------------------------------------------------------------------

// Плоский текст с кэшем: документ на набор в поле поиска не меняется, а
// toPlainText() у заметки в 8.4 МБ стоит 15 мс (замер) — это на КАЖДУЮ букву
// запроса. Ревизия Qt меняется на любой правке документа, по ней кэш и живёт.
const QString& PlainEditView::flatText() {
    const int revision = document()->revision();
    if (revision != flatRevision_) {
        flat_ = toPlainText();
        flatRevision_ = revision;
    }
    return flat_;
}

int PlainEditView::findMatches(const Query& query) {
    matches_.clear();
    capped_ = false;
    current_ = -1;
    query_ = query;
    if (!query.usable()) {
        refreshOverlays();
        return 0;
    }
    // ТЕМ ЖЕ СЧЁТОМ, ЧТО И В ЗАМЕТКЕ (findInText): те же правила про шаг, про
    // пустое совпадение и про регистр — иначе один и тот же запрос давал бы в
    // двух видах разные числа.
    for (const FlatHit& hit :
         findInText(flatText(), query, settings().ui().findMatchLimit(), &capped_))
        matches_.push_back(Match{hit.offset, hit.length, hit.match});
    // ТЕКУЩЕГО ПОКА НЕТ — и это не забывчивость, а правило (то же, что в виде
    // заметки). Куда шагнуть, решает сам шаг: он идёт к ближайшей находке ОТ
    // КАРЕТКИ. Стоило поставить «текущую» здесь — и первый F3 её проскакивал:
    // владелец увидел это после правки («не перехожу на ближайшее вхождение, а
    // перепрыгиваю»), потому что после правки поиск повторяется и «текущая»
    // назначалась заново.
    //
    // Исключение — каретка уже стоит НА находке (вернулись к ней, выделив
    // текст): тогда она и есть текущая, иначе счётчик показывал бы «0/N» при
    // выделенном вхождении.
    const QTextCursor caret = textCursor();
    if (caret.hasSelection()) {
        const int from = qMin(caret.selectionStart(), caret.selectionEnd());
        const int to = qMax(caret.selectionStart(), caret.selectionEnd());
        for (size_t i = 0; i < matches_.size(); ++i)
            if (matches_[i].offset == from && matches_[i].offset + matches_[i].length == to) {
                current_ = int(i);
                break;
            }
    }
    refreshOverlays();
    return int(matches_.size());
}

void PlainEditView::goToMatch(int index) {
    if (matches_.empty()) return;
    const int count = int(matches_.size());
    current_ = ((index % count) + count) % count;
    const Match one = matches_[size_t(current_)];
    QTextCursor at(document());
    at.setPosition(one.offset);
    at.setPosition(one.offset + one.length, QTextCursor::KeepAnchor);
    setTextCursor(at);
    centerCursor();
    refreshOverlays();
}

void PlainEditView::stepMatch(int direction) {
    if (matches_.empty()) return;
    const int count = int(matches_.size());
    if (current_ < 0) {
        // Текущей нет — шаг идёт от КАРЕТКИ: человек только что на что-то
        // смотрел, и прыжок в начало текста был бы неожиданным. Дальше каретки
        // ничего нет — по кругу.
        const int at = textCursor().position();
        int nearest = -1;
        if (direction >= 0) {
            for (int i = 0; i < count; ++i)
                if (matches_[size_t(i)].offset >= at) { nearest = i; break; }
            current_ = nearest >= 0 ? nearest : 0;
        } else {
            for (int i = count - 1; i >= 0; --i)
                if (matches_[size_t(i)].offset + matches_[size_t(i)].length <= at)
                    { nearest = i; break; }
            current_ = nearest >= 0 ? nearest : count - 1;
        }
    } else {
        current_ = (current_ + (direction >= 0 ? 1 : count - 1)) % count;
    }
    QTextCursor at(document());
    at.setPosition(matches_[size_t(current_)].offset);
    at.setPosition(matches_[size_t(current_)].offset + matches_[size_t(current_)].length,
                   QTextCursor::KeepAnchor);
    setTextCursor(at);
    centerCursor();
    refreshOverlays();
}

void PlainEditView::clearMatches() {
    searchSoon_.stop();
    matches_.clear();
    current_ = -1;
    query_ = Query{};
    setExtraSelections({});
}

QString PlainEditView::searchPreset() const { return textCursor().selectedText(); }

void PlainEditView::refreshOverlays() {
    QList<QTextEdit::ExtraSelection> shown;
    // 1. Подсветки наследника (плашки кода в исходнике) — подложкой.
    extraOverlays(shown);

    // 2. НАЙДЕННОЕ — поверх, тоже только видимое.
    if (!matches_.empty()) {
        const int from = cursorForPosition(QPoint(0, 0)).position();
        const int to = cursorForPosition(QPoint(viewport()->width(), viewport()->height()))
                           .position();
        for (size_t i = 0; i < matches_.size(); ++i) {
            const int at = matches_[i].offset;
            const int length = matches_[i].length;
            if (at + length < from || at > to) continue;
            QTextEdit::ExtraSelection one;
            one.cursor = QTextCursor(document());
            one.cursor.setPosition(at);
            one.cursor.setPosition(at + length, QTextCursor::KeepAnchor);
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

bool PlainEditView::replaceCurrentMatch(const QString& with) {
    if (current_ < 0 || size_t(current_) >= matches_.size()) return false;
    const Match one = matches_[size_t(current_)];
    QTextCursor at(document());
    at.setPosition(one.offset);
    at.setPosition(one.offset + one.length, QTextCursor::KeepAnchor);
    at.insertText(expandReplacement(query_, one.match, with));
    setTextCursor(at);
    // Найденное пересчитывается само: позиции за заменой сдвинулись.
    findMatches(query_);
    return true;
}

int PlainEditView::replaceAllMatches(const Query& query, const QString& with) {
    if (!query.usable()) return 0;
    // Замена без потолка: она делает работу, а не показывает число.
    const std::vector<FlatHit> hits = findInText(flatText(), query);
    if (hits.empty()) return 0;
    // ОДНА СКОБКА НА ВСЁ: иначе откатывать пришлось бы по одному вхождению.
    // Идём с конца — передние замены не сдвигают ещё не сделанные.
    QTextCursor edit(document());
    edit.beginEditBlock();
    for (size_t i = hits.size(); i-- > 0;) {
        QTextCursor one(document());
        one.setPosition(hits[i].offset);
        one.setPosition(hits[i].offset + hits[i].length, QTextCursor::KeepAnchor);
        one.insertText(expandReplacement(query, hits[i].match, with));
    }
    edit.endEditBlock();
    clearMatches();
    return int(hits.size());
}

}  // namespace zametti
