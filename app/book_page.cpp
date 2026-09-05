#include "book_page.h"

#include "settings.h"

#include <QAbstractTextDocumentLayout>
#include <QFontMetricsF>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QScopeGuard>
#include <QScrollBar>
#include <QTextBlock>
#include <QTextLayout>
#include <QWheelEvent>

namespace zametti {

namespace {
QFont baseFontOf(qreal zoom, const ZDocStyle& style) {
    QFont font{QString(style.fontFamily())};
    font.setPointSizeF(style.baseFontPoint() * zoom);
    font.setStyleHint(QFont::Serif);
    return font;
}
}  // namespace

BookPage::BookPage(QWidget* parent)
    : NoteView(parent), reading_(settings().readingStyle()) {
    setReadOnly(true);
    // Keys turn pages, Ctrl+C copies, Ctrl+F searches: the page takes focus.
    setFocusPolicy(Qt::StrongFocus);
    applyPalette(*this, /*history=*/false);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setDocument(blank_.getDocument());
    // THE PAGE SNAPS BACK TO ITS START WHEN THE SCROLL RANGE GROWS. Qt lays
    // the document out lazily and tells the scroll bar its range as it goes;
    // a start asked for beyond the range known so far is clamped, and the
    // page would show a line cut at its top for good. Every growth of the
    // range puts the start back where it belongs.
    connect(verticalScrollBar(), &QScrollBar::rangeChanged, this, [this](int, int) {
        if (note_ == nullptr || snapping_) return;
        showStart(start_);
    });
}

void BookPage::showNote(std::shared_ptr<ZNote> note, bool lead) {
    lead_ = lead;
    if (note_ == note) {
        if (lead_) takeOverDocument();
        return;
    }
    note_ = std::move(note);
    start_ = {};
    if (note_ == nullptr) {
        clear();
        return;
    }
    adoptNoteAt(note_->path());
    // THE ONE PERMITTED USE of the hatch: the live document goes to the view
    // to be shown, and nothing else is ever done with it here.
    setDocument(note_->doc().getDocument());
    if (lead_) takeOverDocument();
    else restoreScale();
}

void BookPage::clear() {
    note_.reset();
    start_ = {};
    setDocument(blank_.getDocument());
}

qreal BookPage::pageHeight() const { return viewport()->height(); }

void BookPage::showStart(const PageStart& start) {
    start_ = start;
    if (document() == nullptr || snapping_) return;
    snapping_ = true;
    const auto done = qScopeGuard([this] { snapping_ = false; });
    const QTextBlock block = document()->findBlockByNumber(start.block);
    if (!block.isValid()) return;
    const qreal y = lineSpanOf(*document()->documentLayout(), block, start.line).top;
    verticalScrollBar()->setValue(int(y));
    viewport()->update();
}

PageStart BookPage::topLine() const { return lineAt(verticalScrollBar()->value()); }

PageStart BookPage::lineAt(qreal y) const {
    PageStart out;
    if (document() == nullptr) return out;
    QTextBlock block = blockAtHeight(y);
    if (!block.isValid()) return out;
    const QAbstractTextDocumentLayout* layout = document()->documentLayout();
    for (; block.isValid(); block = block.next()) {
        const QRectF rect = layout->blockBoundingRect(block);
        if (rect.bottom() < y && block.next().isValid()) continue;
        out.block = block.blockNumber();
        out.line = 0;
        const QTextLayout* tl = block.layout();
        if (tl == nullptr) return out;
        for (int i = 0; i < tl->lineCount(); ++i) {
            out.line = i;
            if (lineSpanOf(*layout, block, i).bottom > y) return out;
        }
        return out;
    }
    return out;
}

void BookPage::refreshAppearance() {
    reading_ = settings().readingStyle();
    applyPalette(*this, /*history=*/false);
    restoreScale();
}

QFont BookPage::zoomedBaseFont(qreal zoom) const {
    // A book's document is built with the reading style, so this is the same
    // font the editor shows it with — and the switch costs no relayout. An
    // ordinary note read as a book gets the reading font here and pays one
    // relayout, as a zoom step does.
    return baseFontOf(zoom, *reading_);
}

int BookPage::pagePadding() const {
    // Air above the first line of a page: three quarters of a line.
    return int(QFontMetricsF(zoomedBaseFont(zoom())).height() * 0.75);
}

NoteSearch& BookPage::searchCache() {
    return note_ != nullptr ? note_->search() : NoteView::searchCache();
}

const NoteSearch& BookPage::searchCache() const {
    return note_ != nullptr ? note_->search() : NoteView::searchCache();
}

void BookPage::revealInGolden(const QRectF& place) {
    // A page does not scroll to a place — it turns to the page that holds it.
    if (place.isNull()) return;
    emit revealRequested(lineAt(place.top()));
}

bool BookPage::event(QEvent* event) {
    // The paging keys are ours before the window's shortcuts see them.
    if (event->type() == QEvent::ShortcutOverride) {
        auto* key = static_cast<QKeyEvent*>(event);
        switch (key->key()) {
            case Qt::Key_Space:
            case Qt::Key_Left:
            case Qt::Key_Right:
            case Qt::Key_Up:
            case Qt::Key_Down:
            case Qt::Key_PageUp:
            case Qt::Key_PageDown:
            case Qt::Key_Home:
            case Qt::Key_End:
                if ((key->modifiers() & ~Qt::ShiftModifier & ~Qt::KeypadModifier) == Qt::NoModifier) {
                    event->accept();
                    return true;
                }
                break;
            default:
                break;
        }
    }
    return NoteView::event(event);
}

void BookPage::keyPressEvent(QKeyEvent* event) {
    const Qt::KeyboardModifiers mods = event->modifiers() & ~Qt::KeypadModifier;
    const bool plain = mods == Qt::NoModifier;
    const bool shifted = mods == Qt::ShiftModifier;
    switch (event->key()) {
        case Qt::Key_Space:
            if (plain || shifted) {
                emit pageStepRequested(shifted ? -1 : +1);
                event->accept();
                return;
            }
            break;
        case Qt::Key_Right:
        case Qt::Key_Down:
        case Qt::Key_PageDown:
            if (plain) {
                emit pageStepRequested(+1);
                event->accept();
                return;
            }
            break;
        case Qt::Key_Left:
        case Qt::Key_Up:
        case Qt::Key_PageUp:
            if (plain) {
                emit pageStepRequested(-1);
                event->accept();
                return;
            }
            break;
        case Qt::Key_Home:
        case Qt::Key_End:
            if (plain) {
                emit jumpRequested(event->key() == Qt::Key_End);
                event->accept();
                return;
            }
            break;
        default:
            break;
    }
    // A printing key changes nothing on a page (the widget is read-only), and
    // it must not fall through to the base class's line navigation either.
    if (!event->text().isEmpty() && event->text().at(0).isPrint() &&
        (mods & ~Qt::ShiftModifier) == Qt::NoModifier) {
        event->accept();
        return;
    }
    NoteView::keyPressEvent(event);
}

void BookPage::wheelEvent(QWheelEvent* event) {
    if (event->modifiers() & Qt::ControlModifier) {
        NoteView::wheelEvent(event);   // zoom steps, as everywhere
        return;
    }
    // One notch of the wheel — one spread; the touchpad's fine steps add up
    // to the same notch. No scrolling: a page is a page.
    wheelAccumulated_ += event->angleDelta().y();
    while (wheelAccumulated_ >= 120) {
        wheelAccumulated_ -= 120;
        emit pageStepRequested(-1);
    }
    while (wheelAccumulated_ <= -120) {
        wheelAccumulated_ += 120;
        emit pageStepRequested(+1);
    }
    event->accept();
}

void BookPage::mousePressEvent(QMouseEvent* event) {
    emit activated();
    NoteView::mousePressEvent(event);
}

void BookPage::mouseReleaseEvent(QMouseEvent* event) {
    NoteView::mouseReleaseEvent(event);
    // A drag past the edge auto-scrolls the widget; the page snaps back to
    // its start so that what is shown is still page k and nothing else.
    if (note_ != nullptr && topLine() != start_) showStart(start_);
}

void BookPage::paintEvent(QPaintEvent* event) {
    NoteView::paintEvent(event);
    if (document() == nullptr || note_ == nullptr) return;
    // THE CUT LINE IS HIDDEN. The last line that does not fit whole is the
    // first line of the next page (BookPages); showing its top half here
    // would read as a torn page.
    const int scroll = verticalScrollBar()->value();
    const qreal bottom = scroll + viewport()->height();
    QTextBlock block = blockAtHeight(bottom);
    if (!block.isValid()) return;
    const QTextLayout* tl = block.layout();
    if (tl == nullptr) return;
    const QAbstractTextDocumentLayout& layout = *document()->documentLayout();
    for (int i = 0; i < tl->lineCount(); ++i) {
        const LineSpan span = lineSpanOf(layout, block, i);
        if (span.top < bottom && span.bottom > bottom) {
            QPainter painter(viewport());
            painter.fillRect(QRectF(0.0, span.top - scroll, viewport()->width(),
                                    viewport()->height() - (span.top - scroll)),
                             pageColour());
            return;
        }
    }
}

}  // namespace zametti
