#include "markdown_edit_view.h"

#include "note_view.h"          // applyPalette — палитра у всех видов одна
#include "syntax_highlighter.h"

#include <QAbstractTextDocumentLayout>

#include <QColor>
#include <QFontMetricsF>
#include <QKeyEvent>
#include <QScrollBar>
#include <QTextBlock>
#include <QTextCursor>

#include <algorithm>

namespace zametti {

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
    refreshAppearance();
}

MarkdownEditView::~MarkdownEditView() = default;

void MarkdownEditView::refreshAppearance() {
    applyPalette(*this, /*history=*/false, settings().style());

    // ПОДСВЕТКА — тот же класс ядра, что расцвечивает строки разности
    // (ZSyntaxHighlighterMD). Он и писался под этот режим. Прикрепляется к
    // СВОЕМУ документу: заметка тут ни при чём.
    highlighter_ = std::make_shared<ZSyntaxHighlighterMD>(
        document(), settings().markdownHighlighting(), 0);
    highlighter_->rehighlight();

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
}

void MarkdownEditView::resizeEvent(QResizeEvent* event) {
    QPlainTextEdit::resizeEvent(event);
    applyContentWidth();
    refreshOverlays();
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

}  // namespace

void MarkdownEditView::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape) {
        emit leaveRequested();
        event->accept();
        return;
    }

    const int stop = qMax(1, settings().editor().codeTabWidth());
    const bool tab = event->key() == Qt::Key_Tab && event->modifiers() == Qt::NoModifier;
    const bool backtab = event->key() == Qt::Key_Backtab ||
                         (event->key() == Qt::Key_Tab && event->modifiers() == Qt::ShiftModifier);
    if (!tab && !backtab) {
        QPlainTextEdit::keyPressEvent(event);
        return;
    }

    QTextCursor at = textCursor();
    const LineSpan span = spanOf(at);
    const bool manyLines = span.last > span.first;

    // ОДНА СТРОКА И БЕЗ ВЫДЕЛЕНИЯ — Tab это ОТСТУП ДО СТОПА, а не четыре
    // пробела: набранное должно вставать в колонку, а не рядом с ней.
    if (tab && !manyLines && !at.hasSelection()) {
        const int column = at.position() - at.block().position();
        at.insertText(QString(stop - column % stop, QLatin1Char(' ')));
        setTextCursor(at);
        event->accept();
        return;
    }

    // Много строк (или Shift+Tab) — двигаем строки целиком, одним шагом отмены.
    at.beginEditBlock();
    for (int number = span.first; number <= span.last; ++number) {
        const QTextBlock line = document()->findBlockByNumber(number);
        if (!line.isValid()) continue;
        QTextCursor edit(line);
        if (tab) {
            edit.insertText(QString(stop, QLatin1Char(' ')));
            continue;
        }
        // Снимаем не больше стопа и только пробелы: чужой отступ табами не
        // трогаем — он значим, и превращать его в свой мы не нанимались.
        const QString text = line.text();
        int drop = 0;
        while (drop < stop && drop < text.size() && text.at(drop) == QLatin1Char(' ')) ++drop;
        if (drop == 0) continue;
        edit.setPosition(line.position());
        edit.setPosition(line.position() + drop, QTextCursor::KeepAnchor);
        edit.removeSelectedText();
    }
    at.endEditBlock();
    event->accept();
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
        QTextEdit::ExtraSelection band;
        band.cursor = QTextCursor(block);
        band.format.setBackground(plate);
        band.format.setProperty(QTextFormat::FullWidthSelection, true);
        shown.push_back(band);
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
