#include "book_page.h"

#include "doc_model.h"
#include "key_binding.h"
#include "settings.h"

#include <QAbstractTextDocumentLayout>
#include <QFontMetricsF>
#include <QHelpEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QScopeGuard>
#include <QScrollBar>
#include <QToolTip>
#include <QTextBlock>
#include <QTextFragment>
#include <QTextLayout>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>

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
    // ONE CANVAS, TWO PAGES DRAWN ON IT (owner's wish): no frame around a
    // page and no seam between the two — the spread paints the same page
    // colour under both, and the gap is just paper.
    setFrameShape(QFrame::NoFrame);
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
        // The cover leaf has no start to snap to: the text under it is not shown.
        if (note_ == nullptr || snapping_ || coverShown()) return;
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
    cover_ = QImage();
    setDocument(blank_.getDocument());
}

void BookPage::showCover(const QImage& cover) {
    cover_ = cover;
    start_ = {};
    end_.reset();
    viewport()->update();
}

void BookPage::setEnd(std::optional<PageStart> end) {
    if (end_ == end) return;
    end_ = end;
    viewport()->update();
}

qreal BookPage::pageHeight() const { return viewport()->height(); }

void BookPage::showStart(const PageStart& start) {
    start_ = start;
    if (!cover_.isNull()) {
        cover_ = QImage();
        viewport()->update();
    }
    if (document() == nullptr || snapping_) return;
    snapping_ = true;
    const auto done = qScopeGuard([this] { snapping_ = false; });
    const QTextBlock block = document()->findBlockByNumber(start.block);
    if (!block.isValid()) {
        // THE BLANK LEAF past the end of the book (the right page of the last
        // spread): nothing to scroll to, but the page must repaint as paper
        // — it kept showing the previous right page (owner's report,
        // 07.09.2026).
        viewport()->update();
        return;
    }
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

bool BookPage::blockBookmarked(int block) const {
    return note_ != nullptr && note_->bookmarks().at(block) != nullptr;
}

void BookPage::mouseDoubleClickEvent(QMouseEvent* event) {
    if (const int block = marginBlockAt(event->position()); block >= 0 && note_ != nullptr) {
        emit bookmarkToggleRequested(block);
        event->accept();
        return;
    }
    NoteView::mouseDoubleClickEvent(event);
}

bool BookPage::event(QEvent* event) {
    // The paging keys are ours before the window's shortcuts see them.
    if (event->type() == QEvent::ShortcutOverride) {
        auto* key = static_cast<QKeyEvent*>(event);
        const ZSettings::Editor& keys = settings().editor();
        if (keyEventMatches(*key, keys.bookmarkKey()) || keyEventMatches(*key, keys.nextBookmarkKey()) ||
            keyEventMatches(*key, keys.previousBookmarkKey())) {
            event->accept();
            return true;
        }
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
    {
        const ZSettings::Editor& keys = settings().editor();
        if (keyEventMatches(*event, keys.bookmarkKey())) {
            // The page's first paragraph: the one whose beginning is on it.
            emit bookmarkToggleRequested(start_.line > 0 ? start_.block + 1 : start_.block);
            event->accept();
            return;
        }
        if (keyEventMatches(*event, keys.nextBookmarkKey())) {
            emit bookmarkStepRequested(+1);
            event->accept();
            return;
        }
        if (keyEventMatches(*event, keys.previousBookmarkKey())) {
            emit bookmarkStepRequested(-1);
            event->accept();
            return;
        }
    }
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

QString BookPage::footnoteAt(const QPointF& viewportPos) const {
    if (note_ == nullptr || document() == nullptr || coverShown() || blankLeaf()) return {};
    const QPointF inDocument(viewportPos.x() + horizontalScrollBar()->value(),
                             viewportPos.y() + verticalScrollBar()->value());
    // Above the page's start there is paper, not text (paintEvent).
    if (inDocument.y() < startTopY()) return {};
    const QAbstractTextDocumentLayout& layout = *document()->documentLayout();
    const int exact = layout.hitTest(inDocument, Qt::ExactHit);
    if (exact >= 0) {
        const QString id = note_->doc().footnoteRefAt(exact);
        if (!id.isEmpty()) return note_->doc().footnoteText(id);
    }
    // THE LABEL IS SMALL — a superscript at two thirds of the text — and an
    // exact hit wanted the pointer on the glyph itself (owner's report,
    // 07.09.2026: the plate showed "sometimes"). So the labels of the blocks
    // under the pointer are measured themselves, and half a letter of slack
    // around a label's box counts as a hit. Local: the block or two at the
    // pointer, not the document.
    const qreal slack = QFontMetricsF(font()).horizontalAdvance(QLatin1Char('A')) / 2.0;
    const qreal yEnd = inDocument.y() + slack;
    for (QTextBlock block = blockAtHeight(inDocument.y() - slack); block.isValid();
         block = block.next()) {
        const QRectF blockRect = layout.blockBoundingRect(block);
        if (blockRect.top() > yEnd) break;
        const QTextLayout* tl = block.layout();
        if (tl == nullptr) continue;
        for (QTextBlock::iterator it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment fragment = it.fragment();
            if (!fragment.isValid() || !fragment.charFormat().hasProperty(FootnoteIdProperty)) continue;
            const int from = fragment.position() - block.position();
            const int to = from + fragment.length();
            const QTextLine line = tl->lineForTextPosition(from);
            if (!line.isValid()) continue;
            const qreal x1 = line.cursorToX(from);
            const qreal x2 = line.cursorToX(to);
            const QRectF box(tl->position().x() + std::min(x1, x2), tl->position().y() + line.y(),
                             std::abs(x2 - x1), line.height());
            if (box.adjusted(-slack, -slack, slack, slack).contains(inDocument))
                return note_->doc().footnoteText(
                    fragment.charFormat().stringProperty(FootnoteIdProperty));
        }
    }
    return {};
}

namespace {
// The note as the plate shows it. Rich text: a plain string is shown on one
// line however long it is, a paragraph wraps to the tooltip's width.
QString footnotePlate(const QString& note) {
    QString body = note.toHtmlEscaped();
    body.replace(QLatin1Char('\n'), QLatin1String("<br>"));
    return QStringLiteral("<p>") + body + QStringLiteral("</p>");
}
}  // namespace

bool BookPage::viewportEvent(QEvent* event) {
    // THE NOTE ON HOVER (books2): the pointer resting on a reference shows
    // the note's text; the definitions stay at the end of the book.
    if (event->type() == QEvent::ToolTip) {
        auto* help = static_cast<QHelpEvent*>(event);
        const QString note = footnoteAt(QPointF(help->pos()));
        if (note.isEmpty()) {
            QToolTip::hideText();
            event->ignore();
        } else {
            QToolTip::showText(help->globalPos(), footnotePlate(note), viewport());
            event->accept();
        }
        return true;
    }
    return NoteView::viewportEvent(event);
}

void BookPage::mousePressEvent(QMouseEvent* event) {
    emit activated();
    // The cover leaf has no text to select under the picture; the blank
    // leaf past the end has none at all.
    if (coverShown() || blankLeaf()) {
        event->accept();
        return;
    }
    // A CLICK ON A FOOTNOTE REFERENCE shows the note itself too (brief 18
    // §4): on a touch screen there is no hover.
    if (event->button() == Qt::LeftButton) {
        const QString note = footnoteAt(event->position());
        if (!note.isEmpty()) {
            QToolTip::showText(event->globalPosition().toPoint(), footnotePlate(note), viewport());
            event->accept();
            return;
        }
    }
    NoteView::mousePressEvent(event);
}

void BookPage::mouseReleaseEvent(QMouseEvent* event) {
    NoteView::mouseReleaseEvent(event);
    // A drag past the edge auto-scrolls the widget; the page snaps back to
    // its start so that what is shown is still page k and nothing else.
    if (note_ != nullptr && topLine() != start_) showStart(start_);
}

bool BookPage::blankLeaf() const {
    return note_ != nullptr && document() != nullptr && cover_.isNull() &&
           start_.block >= document()->blockCount();
}

void BookPage::paintEvent(QPaintEvent* event) {
    if (blankLeaf()) {
        // The leaf past the end of the book: paper alone.
        QPainter painter(viewport());
        painter.fillRect(viewport()->rect(), pageColour());
        return;
    }
    if (coverShown()) {
        // THE COVER LEAF: paper and the picture, fitted within the page's
        // padding, centred; the text under it is not drawn.
        QPainter painter(viewport());
        painter.fillRect(viewport()->rect(), pageColour());
        const int pad = pagePadding();
        const QRect area = viewport()->rect().adjusted(pad, pad, -pad, -pad);
        if (area.isEmpty()) return;
        QSize size = cover_.size();
        size.scale(area.size(), Qt::KeepAspectRatio);
        QRect target(QPoint(0, 0), size);
        target.moveCenter(area.center());
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        painter.drawImage(target, cover_);
        return;
    }
    NoteView::paintEvent(event);
    if (document() == nullptr || note_ == nullptr) return;
    // THE PAGE ENDS WHERE THE NEXT ONE BEGINS. Everything from the next
    // page's first line down is covered with paper: the cut line (the line
    // that did not fit whole — showing its top half would read as a torn
    // page) and, when a chapter turns the page early, the heading and the
    // text that would otherwise still fit at the foot of this one.
    const int scroll = verticalScrollBar()->value();
    const qreal bottom = scroll + viewport()->height();
    const QAbstractTextDocumentLayout& layout = *document()->documentLayout();
    qreal maskTop = bottom;
    if (end_.has_value()) {
        const QTextBlock endBlock = document()->findBlockByNumber(end_->block);
        if (endBlock.isValid() && endBlock.layout() != nullptr &&
            end_->line < endBlock.layout()->lineCount())
            maskTop = std::min(maskTop, lineSpanOf(layout, endBlock, end_->line).top);
    }
    if (maskTop >= bottom) {
        QTextBlock block = blockAtHeight(bottom);
        if (block.isValid() && block.layout() != nullptr) {
            const QTextLayout* tl = block.layout();
            for (int i = 0; i < tl->lineCount(); ++i) {
                const LineSpan span = lineSpanOf(layout, block, i);
                if (span.top < bottom && span.bottom > bottom) {
                    maskTop = span.top;
                    break;
                }
            }
        }
    }
    QPainter painter(viewport());
    if (maskTop < bottom)
        painter.fillRect(QRectF(0.0, maskTop - scroll, viewport()->width(),
                                viewport()->height() - (maskTop - scroll)),
                         pageColour());
    // AND THE PAGE BEGINS WHERE IT BEGINS. The last page of a book cannot
    // scroll to its start — the scroll range ends with the document — and
    // showed the tail of the page before above its own first line (owner's
    // report, 07.09.2026: the picture twice, on both pages of the spread).
    // Whatever lies above the start is paper too.
    const qreal startTop = startTopY();
    if (startTop - scroll > 0.5)
        painter.fillRect(QRectF(0.0, 0.0, viewport()->width(), startTop - scroll), pageColour());
}

qreal BookPage::startTopY() const {
    if (document() == nullptr) return 0.0;
    const QTextBlock block = document()->findBlockByNumber(start_.block);
    const QTextLayout* tl = block.isValid() ? block.layout() : nullptr;
    if (tl == nullptr || start_.line >= tl->lineCount()) return 0.0;
    return lineSpanOf(*document()->documentLayout(), block, start_.line).top;
}

}  // namespace zametti
