#include "book_pages.h"

#include "doc_model.h"

#include <QAbstractTextDocumentLayout>
#include <QElapsedTimer>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextLayout>
#include <algorithm>

namespace zametti {

namespace {
// How long one background slice may hold the GUI thread. The full walk of the
// largest book measured 264 ms when it forced the layout; slices this short
// keep the frame smooth and finish the count within a second or two.
constexpr int kSliceMs = 6;
}  // namespace

LineSpan lineSpanOf(const QAbstractTextDocumentLayout& layout, const QTextBlock& block, int line) {
    // Asking the rect lays the block out if it is not yet.
    const QRectF rect = layout.blockBoundingRect(block);
    LineSpan span{rect.top(), rect.bottom()};
    const QTextLayout* tl = block.layout();
    if (tl == nullptr || tl->lineCount() == 0) return span;
    const QTextLine ln = tl->lineAt(std::clamp(line, 0, tl->lineCount() - 1));
    const qreal top = tl->position().y() + ln.y();
    span.top = std::clamp(top, rect.top(), rect.bottom());
    span.bottom = std::clamp(top + ln.height(), span.top, rect.bottom());
    if (span.bottom <= span.top) span.bottom = std::min(rect.bottom(), span.top + 1.0);
    return span;
}

BookPages::BookPages(QObject* parent) : QObject(parent) {
    idle_.setSingleShot(true);
    idle_.setInterval(0);
    connect(&idle_, &QTimer::timeout, this, &BookPages::slice);
}

void BookPages::attach(QTextDocument* document) {
    doc_ = document;
    reset(height_);
}

void BookPages::reset(qreal pageHeight) {
    height_ = pageHeight;
    starts_.clear();
    complete_ = false;
    idle_.stop();
    if (doc_ == nullptr || height_ <= 0.0) return;
    idle_.start();
}

qreal BookPages::yOf(const PageStart& start) const {
    if (doc_ == nullptr) return 0.0;
    const QTextBlock block = doc_->findBlockByNumber(start.block);
    if (!block.isValid()) return 0.0;
    return lineSpanOf(*doc_->documentLayout(), block, start.line).top;
}

PageStart BookPages::lineAt(qreal y) const {
    PageStart out;
    if (doc_ == nullptr) return out;
    const QAbstractTextDocumentLayout* layout = doc_->documentLayout();
    // The hit test knows only what is laid out; walking from there forward
    // forces the rest block by block, as the view's own blockAtHeight does.
    QTextBlock block = doc_->findBlock(layout->hitTest(QPointF(0.0, y), Qt::FuzzyHit));
    if (!block.isValid()) block = doc_->begin();
    for (; block.isValid(); block = block.next()) {
        const QRectF rect = layout->blockBoundingRect(block);
        if (rect.bottom() < y && block.next().isValid()) continue;
        const QTextLayout* tl = block.layout();
        out.block = block.blockNumber();
        out.line = 0;
        if (tl == nullptr) return out;
        for (int i = 0; i < tl->lineCount(); ++i) {
            out.line = i;
            if (lineSpanOf(*layout, block, i).bottom > y) return out;
        }
        return out;
    }
    return out;
}

bool BookPages::extend() {
    if (complete_ || doc_ == nullptr || height_ <= 0.0) return false;
    if (starts_.empty()) {
        starts_.push_back({0, 0});
        return true;
    }
    const PageStart last = starts_.back();
    const qreal limit = yOf(last) + height_;
    const QAbstractTextDocumentLayout* layout = doc_->documentLayout();
    QTextBlock block = doc_->findBlockByNumber(last.block);
    bool first = true;
    for (; block.isValid(); block = block.next(), first = false) {
        // A chapter begins a page of its own (books2): the heading that is
        // not already the page's first line turns the page. The collapsed
        // blank line before it stays at the foot of the previous page.
        if (!first && breakLevel_ > 0 && kindOf(block) == Kind::Heading) {
            const int level = block.blockFormat().headingLevel();
            if (level > 0 && level <= breakLevel_) {
                starts_.push_back({block.blockNumber(), 0});
                return true;
            }
        }
        (void)layout->blockBoundingRect(block);   // lays the block out
        const QTextLayout* tl = block.layout();
        if (tl == nullptr) continue;
        const int lines = tl->lineCount();
        for (int i = first ? last.line : 0; i < lines; ++i) {
            if (lineSpanOf(*layout, block, i).bottom <= limit) continue;
            PageStart next{block.blockNumber(), i};
            if (next == last) {
                // A line taller than the page (a picture): the next page begins
                // right after it, or the book would never turn.
                if (i + 1 < lines) {
                    next.line = i + 1;
                } else {
                    const QTextBlock after = block.next();
                    if (!after.isValid()) {
                        complete_ = true;
                        return false;
                    }
                    next = {after.blockNumber(), 0};
                }
            }
            starts_.push_back(next);
            return true;
        }
    }
    complete_ = true;
    return false;
}

bool BookPages::startOf(int k, PageStart* out) {
    if (k < 0) return false;
    while (int(starts_.size()) <= k && extend()) {}
    if (k >= int(starts_.size())) return false;
    *out = starts_[size_t(k)];
    return true;
}

int BookPages::pageOf(const PageStart& line) {
    if (starts_.empty() && !extend()) return 0;
    while (!complete_ && !(line < starts_.back()) && extend()) {}
    // The last start that is not after the line.
    auto it = std::upper_bound(starts_.begin(), starts_.end(), line);
    if (it == starts_.begin()) return 0;
    return int(it - starts_.begin()) - 1;
}

void BookPages::slice() {
    if (complete_ || doc_ == nullptr) return;
    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < kSliceMs) {
        if (!extend()) break;
    }
    if (complete_) {
        emit countKnown(int(starts_.size()));
        return;
    }
    idle_.start();
}

}  // namespace zametti
