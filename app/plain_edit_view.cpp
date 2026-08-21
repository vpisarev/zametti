#include "plain_edit_view.h"

#include "note_view.h"   // applyPalette, caretShouldBeDrawn — правила у всех видов одни
#include "settings.h"

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
    QRect at = cursorRect();
    at.setWidth(caretPixelWidth(settings().style().caretWidth(), zoom_));
    painter.fillRect(at, settings().style().caretColor());
}

// --- облик, масштаб, поля -----------------------------------------------------

void PlainEditView::refreshAppearance() {
    applyPalette(*this, /*history=*/false, settings().style());
    applyZoom(zoom_);   // шрифт, стоп табуляции и поля — одним местом
}

int PlainEditView::tabStop() const { return qMax(1, settings().editor().codeTabWidth()); }

QColor PlainEditView::wrapMarkColor() const { return settings().markdownHighlighting().comment(); }

void PlainEditView::applyZoom(qreal zoom) {
    const ZDocStyle& style = settings().style();
    zoom_ = qBound(settings().ui().zoomMin(), zoom, settings().ui().zoomMax());

    // ГАРНИТУРА КОДА: плоский текст читают как код — по колонкам, и
    // пропорциональный шрифт сбил бы и таблицы, и отступы.
    QFont font(style.codeFamily());
    font.setPointSizeF(style.baseFontPoint() * zoom_);
    setFont(font);
    document()->setDefaultFont(font);
    // Стоп табуляции — тот же, которым Tab ставит пробелы: набранное и старые
    // литеральные табы обязаны рисоваться одинаково. Считается от НЫНЕШНЕГО
    // шрифта: с масштабом стоп обязан расти вместе с буквами.
    setTabStopDistance(tabStop() * QFontMetricsF(font).horizontalAdvance(QLatin1Char(' ')));

    applyContentWidth();
    refreshOverlays();
}

void PlainEditView::applyContentWidth() {
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

void PlainEditView::resizeEvent(QResizeEvent* event) {
    QPlainTextEdit::resizeEvent(event);
    applyContentWidth();
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

void PlainEditView::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape) {
        emit leaveRequested();
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

int PlainEditView::findMatches(const QString& text, bool caseSensitive) {
    matches_.clear();
    current_ = -1;
    needleText_ = text;
    caseSensitive_ = caseSensitive;
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
    // стоит, а не с начала текста.
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

void PlainEditView::stepMatch(int direction) {
    if (matches_.empty()) return;
    const int count = int(matches_.size());
    current_ = current_ < 0 ? 0 : (current_ + (direction >= 0 ? 1 : count - 1)) % count;
    QTextCursor at(document());
    at.setPosition(matches_[size_t(current_)]);
    at.setPosition(matches_[size_t(current_)] + needle_, QTextCursor::KeepAnchor);
    setTextCursor(at);
    centerCursor();
    refreshOverlays();
}

void PlainEditView::clearMatches() {
    matches_.clear();
    current_ = -1;
    needle_ = 0;
    needleText_.clear();
    setExtraSelections({});
}

QString PlainEditView::searchPreset() const { return textCursor().selectedText(); }

void PlainEditView::refreshOverlays() {
    QList<QTextEdit::ExtraSelection> shown;
    // 1. Подсветки наследника (плашки кода в исходнике) — подложкой.
    extraOverlays(shown);

    // 2. НАЙДЕННОЕ — поверх, тоже только видимое.
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

bool PlainEditView::replaceCurrentMatch(const QString& with) {
    if (current_ < 0 || size_t(current_) >= matches_.size()) return false;
    QTextCursor at(document());
    at.setPosition(matches_[size_t(current_)]);
    at.setPosition(matches_[size_t(current_)] + needle_, QTextCursor::KeepAnchor);
    at.insertText(with);
    setTextCursor(at);
    // Найденное пересчитывается само: позиции за заменой сдвинулись.
    const QString needle = needleText_;
    findMatches(needle, caseSensitive_);
    return true;
}

int PlainEditView::replaceAllMatches(const QString& text, bool caseSensitive, const QString& with) {
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
