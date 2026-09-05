// BookPages — the table of page starts of a book shown page by page (brief 18).
//
// A page is what fits into the viewport: page k+1 begins at the first line
// that did not fit whole on page k (the brief's rule: viewport height, aligned
// to a line top, the cut line repeated on the next page). Starts are kept as
// (block, line) pairs, not pixels: Qt's layout is lazy and refines the heights
// of far blocks as it goes, so a pixel remembered early would lie later; the
// pixel of a start is asked from the layout when it is needed.
//
// The table is built ON DEMAND (paging forward computes the next start from
// the last known one) and, for the total count, IN THE BACKGROUND by slices
// of a timer — a walk over every line of a two-megabyte book is a few
// milliseconds once the layout is done, and a quarter of a second when the
// walk itself has to force the layout (measured, probe 18b.0), which is why
// it never runs inside a frame. Zoom, resize, a rebuilt document, an edit —
// all reset it; the view keeps its place by (block, line) and asks again.
#ifndef ZAMETTI_BOOK_PAGES_H
#define ZAMETTI_BOOK_PAGES_H

#include <QObject>
#include <QTimer>
#include <vector>

class QAbstractTextDocumentLayout;
class QTextBlock;
class QTextDocument;

namespace zametti {

struct PageStart {
    int block = 0;
    int line = 0;
    bool operator==(const PageStart& o) const { return block == o.block && line == o.line; }
    bool operator!=(const PageStart& o) const { return !(*this == o); }
    bool operator<(const PageStart& o) const {
        return block != o.block ? block < o.block : line < o.line;
    }
};

// THE SPAN OF A LINE ON THE PAGE, in document y. Not the raw QTextLine: a
// block whose line height is fixed smaller than its text (the collapsed empty
// line of a book, one pixel) gets its text line placed ABOVE the block — a
// negative y — and a page begun there would show the tail of the line before
// it. The span is the line clipped to the block's own rectangle.
struct LineSpan {
    qreal top = 0.0;
    qreal bottom = 0.0;
};
LineSpan lineSpanOf(const QAbstractTextDocumentLayout& layout, const QTextBlock& block, int line);

class BookPages : public QObject {
    Q_OBJECT

public:
    explicit BookPages(QObject* parent = nullptr);

    // The document whose layout is walked; null — nothing to page.
    void attach(QTextDocument* document);
    // Forget everything and start over with this page height (pixels of the
    // viewport). The background walk restarts on the next idle slice.
    void reset(qreal pageHeight);
    qreal pageHeight() const { return height_; }

    // Start of page k (0-based), computing up to it when needed; false — the
    // book has fewer pages.
    bool startOf(int k, PageStart* out);
    // The page that shows the given line at or below its top: the last page
    // whose start is not after the line. Computes as far as needed.
    int pageOf(const PageStart& line);
    // Number of pages, or -1 while the background walk has not reached the end.
    int count() const { return complete_ ? int(starts_.size()) : -1; }
    bool complete() const { return complete_; }
    // Document y of a start, asked from the layout now (forces it that far).
    qreal yOf(const PageStart& start) const;
    // The line that contains the document y (the line whose span covers it,
    // else the nearest one below); {0,0} for an empty document.
    PageStart lineAt(qreal y) const;

signals:
    // The end was reached: count() is known. Emitted once per reset.
    void countKnown(int count);

private:
    // One more start after the last known; false at the end of the book.
    bool extend();
    void slice();

    QTextDocument* doc_ = nullptr;
    qreal height_ = 0.0;
    std::vector<PageStart> starts_;
    bool complete_ = false;
    QTimer idle_;
};

}  // namespace zametti

#endif  // ZAMETTI_BOOK_PAGES_H
