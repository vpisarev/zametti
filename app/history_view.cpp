#include "history_view.h"

#include "doc_model.h"
#include "settings.h"

#include <QAbstractTextDocumentLayout>
#include <QFontMetricsF>
#include <QKeyEvent>
#include <QMimeData>
#include <QPainter>
#include <QScrollBar>
#include <QTextBlock>
#include <QTextDocumentFragment>
#include <QTextFrame>
#include <QTextLayout>
#include <QTimer>
#include <QVBoxLayout>

namespace zametti {

// --- DiffTextView -----------------------------------------------------------

DiffTextView::DiffTextView(QWidget* parent) : NoteView(parent) {
    setReadOnly(true);
    setFocusPolicy(Qt::StrongFocus);
    // Поле тонировано как прошлое: человек видит, что перед ним не заметка, ещё
    // до того, как прочтёт баннер.
    applyPalette(*this, /*history=*/true);
    setDocument(shown_.getDocument());
}

void DiffTextView::attach(std::shared_ptr<ZNoteTimeline> timeline) {
    // Место снимается ДО подмены — по строке слепка у верхней кромки: у другой
    // базы другие блоки, а строка слепка та же; у другого слепка точного места
    // нет, но номер строки от слепка к слепку меняется мало, и вернуться
    // примерно туда же полезнее, чем начинать с начала (просьба владельца).
    const bool same = timeline == timeline_ && timeline != nullptr;
    const TopAnchor anchor = same ? topAnchor() : TopAnchor{};
    const int keepLine = same ? lineOnTop() : -1;
    timeline_ = std::move(timeline);
    present(keepLine, anchor.offset);
}

void DiffTextView::detach() {
    timeline_.reset();
    present(-1, 0);
}

void DiffTextView::present(int keepLine, int keepOffset) {
    // Поиск держит курсоры в прежнем документе; при подмене вид их снимает
    // сам (NoteView::setDocument), а запрос перезадаём на новом — иначе смена
    // базы гасила бы поиск молча.
    QString query;
    bool caseSensitive = false;
    if (timeline_ != nullptr) {
        query = timeline_->search().text();
        caseSensitive = timeline_->search().caseSensitive();
    }
    ZDocument next = timeline_ != nullptr ? timeline_->document() : ZDocument();
    if (!next.sameHandle(shown_)) {
        ZDocument previous = shown_;
        shown_ = next;
        setDocument(shown_.getDocument());
        // Документ подменили — вернуть ему масштаб (сборщик о масштабе не знает).
        restoreScale();
        applyContentWidth();
        retire(std::move(previous));
    }
    document()->setModified(false);
    if (timeline_ == nullptr) return;
    if (keepLine >= 0) {
        const int block = timeline_->blockOfAfterLine(keepLine);
        if (block >= 0) scrollToBlockTop(block, keepOffset);
    }
    if (!query.isEmpty()) findMatches(query, caseSensitive);
    viewport()->update();
}

void DiffTextView::retire(ZDocument previous) {
    // НИ ОДИН ДОКУМЕНТ НЕ УНИЧТОЖАЕТСЯ СИНХРОННО: подмена идёт из обработчиков
    // событий, и Qt трогает старый документ ещё долю секунды после нас.
    retiring_.push_back(std::move(previous));
    QTimer::singleShot(0, this, [this] { retiring_.clear(); });
}

int DiffTextView::lineOnTop() {
    if (timeline_ == nullptr) return -1;
    const TopAnchor anchor = topAnchor();
    if (anchor.block < 0) return -1;
    return timeline_->afterLineOfBlock(anchor.block);
}

bool DiffTextView::stepChange(bool forward) {
    if (timeline_ == nullptr) return false;
    const int blocks = document()->blockCount();
    if (blocks <= 0) return false;
    const int from = textCursor().blockNumber();
    // По кругу: дошли до края — начинаем сначала. Ходьба по изменениям без
    // круга каждый раз упирается в конец и молчит.
    for (int step = 1; step <= blocks; ++step) {
        const int at = ((forward ? from + step : from - step) % blocks + blocks) % blocks;
        if (timeline_->markOfBlock(at) == diff::Mark::Same) continue;
        // Соседние блоки одного изменения — одно место: встаём на его начало,
        // иначе F4 шагал бы по строкам внутри одного правленого куска.
        int start = at;
        if (forward)
            while (start > 0 && timeline_->markOfBlock(start - 1) != diff::Mark::Same &&
                   start - 1 != from)
                --start;
        int end = start;
        while (end + 1 < blocks && timeline_->markOfBlock(end + 1) != diff::Mark::Same) ++end;
        // МЕСТО ПОКАЗЫВАЕТСЯ ВЫДЕЛЕНИЕМ: каретки в поле только для чтения не
        // видно, и без выделения было непонятно, куда именно привёл F4
        // (замечание владельца). Выделяется весь кусок — все соседние
        // тронутые строки; переход — в золотое сечение окна.
        const QTextBlock first = document()->findBlockByNumber(start);
        const QTextBlock last = document()->findBlockByNumber(end);
        if (!first.isValid() || !last.isValid()) return false;
        QTextCursor place(first);
        place.setPosition(last.position() + last.length() - 1, QTextCursor::KeepAnchor);
        setTextCursor(place);
        const QAbstractTextDocumentLayout* layout = document()->documentLayout();
        revealInGolden(layout->blockBoundingRect(first).united(layout->blockBoundingRect(last)));
        return true;
    }
    return false;
}

NoteSearch& DiffTextView::searchCache() {
    return timeline_ != nullptr ? timeline_->search() : NoteView::searchCache();
}

const NoteSearch& DiffTextView::searchCache() const {
    return timeline_ != nullptr ? timeline_->search() : NoteView::searchCache();
}

void DiffTextView::paintBlockMargin(QPainter& painter, const QTextBlock& block, const QRectF& rect) {
    const int mark = diffMarkOf(block);
    if (mark != int(diff::Mark::Added) && mark != int(diff::Mark::Removed)) return;
    // Глиф стоит на поле, ПЕРЕД строкой, у её первой физической строки: у
    // логической строки, перенесённой мягко, знак один — она одна.
    // Прямоугольник блока начинается ПОСЛЕ поля документа (rect.left() — это и
    // есть левое поле корневой рамки): поле под глиф лежит прямо слева от него.
    const ZDocStyle& look = docStyle();
    const qreal gutter = ZDocument::diffGutterWidth(look);
    const QRectF cell(rect.left() - gutter, rect.top(), gutter,
                      block.layout() != nullptr && block.layout()->lineCount() > 0
                          ? block.layout()->lineAt(0).height()
                          : rect.height());
    // Тем же шрифтом, что и строки: гарнитура кода, ступень diffStep, масштаб вида.
    QFont font = baseFont();
    if (!look.codeFamily().isEmpty()) font.setFamily(QString(look.codeFamily()));
    font.setPointSizeF(font.pointSizeF() * fontStepFactor(look.diffStep()));
    // Не влезает в поле (крупный масштаб) — ужимаем: поле в пикселях и от
    // масштаба не растёт, а глиф растёт.
    const qreal advance = QFontMetricsF(font).horizontalAdvance(QLatin1Char('+'));
    if (advance > gutter * 0.8 && advance > 0.0)
        font.setPointSizeF(qMax(4.0, font.pointSizeF() * gutter * 0.8 / advance));
    painter.setFont(font);
    painter.setPen(mark == int(diff::Mark::Added) ? look.diffAdded() : look.diffRemoved());
    painter.drawText(cell, Qt::AlignCenter,
                     mark == int(diff::Mark::Added) ? QStringLiteral("+") : QStringLiteral("−"));
}

bool DiffTextView::event(QEvent* event) {
    // Esc и Ctrl+Z/Ctrl+Shift+Z — наши: ярлыки окна перехватили бы их раньше
    // keyPressEvent (Esc в окне закрывает панели), а в режиме истории они
    // значат «к текущей версии» и шаги по слепкам.
    if (event->type() == QEvent::ShortcutOverride) {
        auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Escape || key->matches(QKeySequence::Undo) ||
            key->matches(QKeySequence::Redo)) {
            event->accept();
            return true;
        }
    }
    return NoteView::event(event);
}

void DiffTextView::keyPressEvent(QKeyEvent* event) {
    // В слепке клавиши работают иначе: править нечего, зато ходить по истории
    // и копировать из неё — можно, ради этого режим и заведён.
    if (event->matches(QKeySequence::Undo)) {   // шаг в более старое
        emit stepBackRequested();
        event->accept();
        return;
    }
    if (event->matches(QKeySequence::Redo)) {   // шаг в более новое и в живое
        emit stepForwardRequested();
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_Escape) {
        emit leaveRequested();
        event->accept();
        return;
    }
    // Печатающая клавиша не восстанавливает ничего (решение владельца:
    // случайное нажатие при попытке выделить не должно менять режим).
    if (!event->text().isEmpty() && event->text().at(0).isPrint() &&
        (event->modifiers() & ~Qt::ShiftModifier) == Qt::NoModifier) {
        emit editRefused();
        event->accept();
        return;
    }
    // Всё остальное — базовому виджету: перемещение каретки, выделение,
    // Ctrl+C. Правки он и сам не пропустит, поле только для чтения.
    NoteView::keyPressEvent(event);
}

QMimeData* DiffTextView::createMimeDataFromSelection() const {
    auto* data = new QMimeData;
    // Строки как есть: markdown-исходник слепка, без html с заливками строк.
    data->setText(textCursor().selection().toPlainText());
    return data;
}

// --- HistoryView ------------------------------------------------------------

HistoryView::HistoryView(QWidget* parent) : QWidget(parent) {
    banner_ = new HistoryBanner(this);
    text_ = new DiffTextView(this);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(banner_);
    layout->addWidget(text_, 1);
    setFocusProxy(text_);

    connect(banner_, &HistoryBanner::leaveRequested, this, &HistoryView::leaveRequested);
    connect(banner_, &HistoryBanner::restoreRequested, this, &HistoryView::restoreRequested);
    connect(banner_, &HistoryBanner::baseChanged, this, &HistoryView::baseChanged);
    connect(text_, &DiffTextView::leaveRequested, this, &HistoryView::leaveRequested);
    connect(text_, &DiffTextView::stepBackRequested, this, &HistoryView::stepBackRequested);
    connect(text_, &DiffTextView::stepForwardRequested, this, &HistoryView::stepForwardRequested);
    connect(text_, &DiffTextView::editRefused, this, [this] {
        banner_->flashRestore();
        emit editRefused();
    });
}

void HistoryView::attach(std::shared_ptr<ZNoteTimeline> timeline) {
    text_->attach(std::move(timeline));
    syncBanner();
}

void HistoryView::detach() { text_->detach(); }

void HistoryView::refresh() {
    text_->attach(text_->timeline());
    syncBanner();
}

void HistoryView::syncBanner() {
    const std::shared_ptr<ZNoteTimeline> tl = text_->timeline();
    if (tl == nullptr || !tl->isOpen()) return;
    banner_->setBaseIsFresh(tl->baseIsFresh());
    banner_->setSnapshot(tl->snapshotTime(), tl->snapshotKind(), tl->changedLines());
}

}  // namespace zametti
