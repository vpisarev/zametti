#include "zbook_view.h"

#include "settings.h"
#include "zapp.h"

#include <QDir>
#include <QFileInfo>
#include <QFontMetricsF>
#include <QPalette>
#include <QResizeEvent>
#include <QTextBlock>
#include <QTextDocument>
#include <algorithm>
#include <array>

namespace zametti {

ZBookView::ZBookView(QWidget* parent) : QWidget(parent) {
    for (int i = 0; i < 2; ++i) {
        auto* page = new BookPage(this);
        pages_.push_back(page);
        connect(page, &BookPage::pageStepRequested, this, &ZBookView::pageStep);
        connect(page, &BookPage::jumpRequested, this, &ZBookView::jump);
        connect(page, &BookPage::revealRequested, this, &ZBookView::showLine);
        connect(page, &NoteView::zoomStepRequested, this, &ZBookView::zoomStepRequested);
        connect(page, &BookPage::bookmarkToggleRequested, this, &ZBookView::bookmarkToggleRequested);
        connect(page, &BookPage::bookmarkStepRequested, this, &ZBookView::bookmarkStepRequested);
        connect(page, &BookPage::activated, this, [this, i] { active_ = i; });
    }
    pages_[1]->hide();
    setFocusProxy(pages_[0]);
    paintCanvas();
    // An edit of the document (a bookmark does not edit; marks will) shifts
    // the lines: the table is counted anew once the typing pauses.
    edited_.setSingleShot(true);
    edited_.setInterval(300);
    connect(&edited_, &QTimer::timeout, this, [this] {
        resetTable();
        showSpread(spreadOfPlace());
    });
    connect(&table_, &BookPages::countKnown, this, [this](int) { announce(); });
}

qreal ZBookView::charUnit() const {
    return QFontMetricsF(pages_[0]->font()).horizontalAdvance(QLatin1Char('A'));
}

void ZBookView::showNote(std::shared_ptr<ZNote> note) {
    if (note == nullptr) {
        clear();
        return;
    }
    if (note_ == note) {
        reclaim();
        return;
    }
    if (note_ != nullptr) clear();
    note_ = std::move(note);
    // The follower first, the lead last: the last registration of the object
    // handlers wins, and it must be the lead's.
    pages_[1]->showNote(note_, /*lead=*/false);
    pages_[0]->showNote(note_, /*lead=*/true);
    table_.attach(pages_[0]->document());
    contents_ = connect(pages_[0]->document(), &QTextDocument::contentsChanged, this,
                        [this] { edited_.start(); });
    const CaretSpot spot = ZApp::instance().state().caretOf(note_->id());
    anchor_ = PageStart{spot.readingBlock, spot.readingLine};
    active_ = 0;
    // The cover (books2): the picture named by the header, decoded through
    // the application's cache; a fresh book opens on it.
    cover_ = QImage();
    const QString coverName = note_->bookCover();
    if (!coverName.isEmpty()) {
        const QString path = QFileInfo(note_->path()).dir().filePath(coverName);
        // info() registers the file (header only), pixels() decodes it.
        ZImageCache& images = ZApp::instance().images();
        if (images.info(path) != nullptr)
            if (const QImage* image = images.pixels(path); image != nullptr && !image->isNull())
                cover_ = *image;
    }
    first_ = hasCover() && anchor_ == PageStart{} ? -1 : 0;
    relayoutPages();
}

int ZBookView::alignedSpread(int page) const {
    if (shown_ != 2) return page;
    const int base = lowestSpread();
    return page - (page - base) % 2;
}

int ZBookView::spreadOfPlace() {
    if (first_ == -1 && hasCover()) return -1;
    return alignedSpread(table_.pageOf(anchor_));
}

void ZBookView::rememberPlace() {
    if (note_ == nullptr) return;
    ZApp::instance().state().rememberReading(note_->id(), anchor_.block, anchor_.line);
}

void ZBookView::clear() {
    if (note_ == nullptr) return;
    rememberPlace();
    disconnect(contents_);
    edited_.stop();
    table_.attach(nullptr);
    // The leaving views detach first (Qt drops the paint device of the
    // layout a view leaves): both pages go back to their blank documents
    // before the editor's turn.
    pages_[0]->clear();
    pages_[1]->clear();
    note_.reset();
    cover_ = QImage();
    first_ = 0;
}

BookPage& ZBookView::activePage() {
    if (active_ >= shown_) active_ = 0;
    return *pages_[size_t(active_)];
}

void ZBookView::relayoutPages() {
    const ZSettings::Reading& reading = settings().reading();
    const qreal unit = charUnit();
    const int gap = int(reading.pageGap() * unit);
    int wanted = reading.pagesPerSpread();
    if (wanted == 0) {
        const qreal least = reading.minPageWidth() * unit;
        wanted = width() >= 2 * least + gap ? 2 : 1;
    }
    if (wanted != shown_) {
        shown_ = wanted;
        pages_[1]->setVisible(shown_ == 2);
        if (active_ >= shown_) active_ = 0;
    }
    // THE PAGES ARE EXACTLY AS WIDE AS EACH OTHER — by hand, not by a layout.
    // They share one document, and the document has one text width: a layout
    // splitting an odd width would give the two pages viewports a pixel
    // apart, and each would lay the document out for its own on every
    // resize, in whichever order the events came (caught by the symmetry
    // shot: justified lines a pixel different after a round trip).
    int pageWidth = (width() - gap * (shown_ - 1)) / shown_;
    // THE SPREAD HUGS THE GUTTER (owner's wish, 06.09.2026): a page is no wider
    // than its column with the two side margins, and the spare width of a wide
    // window goes outside the spread, not between the columns. Halving the
    // window gave two half-window pages, each centring its column in its own
    // half — 330 px of paper between the columns at 1600 px, and the gap
    // setting hardly mattered. A single page keeps the whole width: its
    // column is centred by the view as before.
    if (shown_ == 2) {
        const ZDocStyle& look = pages_[0]->readingLook();
        if (look.maxContentWidth() > 0.0) {
            const int hugged = int((look.maxContentWidth() + 2.0 * look.sideMargin()) * unit);
            pageWidth = std::min(pageWidth, std::max(hugged, 1));
        }
    }
    const int span = pageWidth * shown_ + gap * (shown_ - 1);
    int x = (width() - span) / 2;
    for (int i = 0; i < shown_; ++i) {
        pages_[size_t(i)]->setGeometry(x, 0, pageWidth, height());
        x += pageWidth + gap;
    }
    resetTable();
    if (note_ != nullptr) showSpread(spreadOfPlace());
}

int ZBookView::chapterLevel() const {
    // THE LEVEL OF THE CHAPTERS (owner's rule, 06.09.2026): the shallowest
    // heading level that occurs more than once. An fb2 book has one H1 (the
    // title) and its chapters as H2 — both turn the page; a manual with many
    // H1 parts and H2 sections in them turns the page at the parts only.
    // Nothing repeats — the first level alone.
    if (note_ == nullptr) return 0;
    std::array<int, 6> perLevel{};
    for (const ZDocument::OutlineEntry& entry : note_->outline())
        if (entry.level >= 1 && entry.level <= 6) ++perLevel[size_t(entry.level - 1)];
    for (int level = 1; level <= 6; ++level)
        if (perLevel[size_t(level - 1)] >= 2) return level;
    return 1;
}

void ZBookView::resetTable() {
    // The height the layout settled on: the pages are laid out by now, and
    // both have the same one.
    // Headings down to reading.pageBreakLevel turn the page, and the book's
    // chapters do in any case (a level deeper than that is rare, not wrong).
    table_.setBreakLevel(std::max(chapterLevel(), settings().reading().pageBreakLevel()));
    table_.reset(pages_[0]->pageHeight());
}

void ZBookView::showSpread(int first) {
    if (note_ == nullptr) return;
    if (first < lowestSpread()) first = lowestSpread();
    // A page shows its text up to the next page's start (BookPage::setEnd).
    const auto endOf = [this](int page) {
        PageStart next;
        std::optional<PageStart> end;
        if (table_.startOf(page + 1, &next)) end = next;
        return end;
    };
    if (first == -1) {
        // The cover leaf: the picture on the left, page 0 on the right.
        first_ = -1;
        anchor_ = PageStart{};
        pages_[0]->showCover(cover_);
        if (shown_ == 2) {
            PageStart start;
            if (table_.startOf(0, &start)) {
                pages_[1]->showStart(start);
                pages_[1]->setEnd(endOf(0));
            }
        }
        announce();
        return;
    }
    PageStart start;
    if (!table_.startOf(first, &start)) {
        // Past the end: the last spread.
        while (first > 0 && !table_.startOf(first, &start)) --first;
        if (!table_.startOf(first, &start)) return;
    }
    first_ = first;
    anchor_ = start;
    pages_[0]->showStart(start);
    pages_[0]->setEnd(endOf(first));
    if (shown_ == 2) {
        PageStart next;
        if (table_.startOf(first + 1, &next)) {
            pages_[1]->showStart(next);
            pages_[1]->setEnd(endOf(first + 1));
        } else {
            // The book ends on the left page: the right one is a blank leaf,
            // shown past the end so that no text repeats.
            pages_[1]->showStart(PageStart{pages_[0]->document()->blockCount(), 0});
            pages_[1]->setEnd(std::nullopt);
        }
    }
    announce();
}

void ZBookView::pageStep(int delta) {
    if (note_ == nullptr) return;
    int next = first_ + delta * shown_;
    if (next < lowestSpread()) next = lowestSpread();
    if (next >= 0) {
        PageStart probe;
        if (!table_.startOf(next, &probe)) return;   // already on the last spread
    }
    showSpread(next);
}

void ZBookView::jump(bool toEnd) {
    if (note_ == nullptr) return;
    if (!toEnd) {
        showSpread(lowestSpread());
        return;
    }
    // The end is known only once the table is: count it now, on demand.
    PageStart probe;
    int last = std::max(first_, 0);
    while (table_.startOf(last + 1, &probe)) ++last;
    showSpread(alignedSpread(last));
}

void ZBookView::showLine(const PageStart& line) {
    if (note_ == nullptr) return;
    const int page = alignedSpread(table_.pageOf(line));
    // Already on view — the pages stay put (as the editor keeps its place
    // when the hit is on screen).
    if (page == first_) {
        announce();
        return;
    }
    showSpread(page);
}

void ZBookView::showBlock(int block) { showLine(PageStart{block, 0}); }

bool ZBookView::blockOnSpread(int block) {
    if (note_ == nullptr) return false;
    return alignedSpread(table_.pageOf(PageStart{block, 0})) == first_;
}

void ZBookView::reclaim() {
    if (note_ == nullptr) return;
    pages_[0]->showNote(note_, /*lead=*/true);
    resetTable();
    showSpread(spreadOfPlace());
}

void ZBookView::paintCanvas() {
    // The paper under both pages and the gap between them: the page colour
    // of the pages themselves, so the spread reads as one sheet.
    QPalette canvas = palette();
    canvas.setColor(QPalette::Window, pages_[0]->palette().color(QPalette::Base));
    setPalette(canvas);
    setAutoFillBackground(true);
}

void ZBookView::refreshAppearance() {
    for (BookPage* page : pages_) page->refreshAppearance();
    paintCanvas();
    if (note_ == nullptr) return;
    pages_[0]->takeOverDocument();
    relayoutPages();
}

void ZBookView::applyZoom(qreal zoom) {
    for (BookPage* page : pages_) page->setZoom(zoom);
    if (note_ != nullptr) pages_[0]->takeOverDocument();
    relayoutPages();
}

qreal ZBookView::zoom() const { return pages_[0]->zoom(); }

void ZBookView::refreshMarks() {
    for (BookPage* page : pages_) page->viewport()->update();
}

void ZBookView::clearMatches() {
    for (BookPage* page : pages_) page->clearMatches();
}

void ZBookView::restorePlace(const PageStart& place) {
    anchor_ = place;
    first_ = 0;
    if (note_ != nullptr) showSpread(spreadOfPlace());
}

void ZBookView::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    // The pages get their new size through the layout first; the table is
    // counted for the settled height, on the next turn of the loop.
    QTimer::singleShot(0, this, [this] { relayoutPages(); });
}

void ZBookView::announce() {
    if (note_ == nullptr) return;
    if (first_ == -1) {
        // The cover leaf: page 0 in the status line means the cover.
        emit positionChanged(QString(), 0, table_.count(), 0);
        return;
    }
    const QTextDocument* doc = pages_[0]->document();
    const QTextBlock top = doc->findBlockByNumber(anchor_.block);
    const int chars = std::max(1, doc->characterCount() - 1);
    const int percent = top.isValid() ? int(100.0 * top.position() / chars) : 0;
    // THE CHAPTER IS THE LAST ONE THAT BEGAN BEFORE THE TOP OF THE RIGHT PAGE
    // (owner's rule, 05.09.2026): a story starting anywhere on the left page
    // — at its top, after the collapsed blank line, or halfway down — is the
    // story being read. With one page the boundary is the page's bottom. So
    // the block just before the next page's start names the chapter; a block
    // cut by the boundary began on this side of it and counts.
    int named = doc->blockCount() - 1;
    PageStart boundary;
    if (table_.startOf(first_ + 1, &boundary))
        named = boundary.line > 0 ? boundary.block : std::max(anchor_.block, boundary.block - 1);
    emit positionChanged(chapterPath(named), first_ + 1, table_.count(),
                         std::clamp(percent, 0, 100));
}

QString ZBookView::chapterPath(int block) {
    // THE PATH OF HEADINGS (owner's wish, 07.09.2026): the chapter and up to
    // two of its ancestors — «Книга первая · Часть первая · VI», not «VI»
    // alone. From the outline cache, by a binary search over the blocks: the
    // walk back over the document (headingAbove) is gone from the status.
    const std::vector<ZDocument::OutlineEntry>& outline = note_->outline();
    auto it = std::upper_bound(outline.begin(), outline.end(), block,
                               [](int b, const ZDocument::OutlineEntry& e) { return b < e.block; });
    if (it == outline.begin()) return {};
    --it;
    QStringList parts{it->text};
    int level = it->level;
    int up = 0;
    for (auto j = it; j != outline.begin() && up < kChapterPathUp;) {
        --j;
        if (j->level < level) {
            parts.prepend(j->text);
            level = j->level;
            ++up;
        }
    }
    return parts.join(QStringLiteral(" · "));
}

}  // namespace zametti
