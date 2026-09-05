// ZBookView — the spread: one or two pages of a book side by side (brief 18).
//
// The owner reads on laptops and tablets in landscape, and wants two pages
// like an open book. The spread owns the table of page starts (BookPages) and
// as many BookPage views as fit — two when the window is wide enough for two
// columns of reading.minPageWidth, one otherwise, or as the settings say —
// all over the SAME live document of the note the editor owns. The left page
// leads (claims the document); the right one follows. Keys, the wheel and the
// search turn spreads; the place is kept as (block, line) of the left page
// and survives zoom, resize and a rebuilt document.
#ifndef ZAMETTI_ZBOOK_VIEW_H
#define ZAMETTI_ZBOOK_VIEW_H

#include "book_page.h"
#include "book_pages.h"
#include "znote.h"

#include <QHash>
#include <QTimer>
#include <QWidget>
#include <memory>
#include <vector>

namespace zametti {

class ZBookView : public QWidget {
    Q_OBJECT

public:
    explicit ZBookView(QWidget* parent = nullptr);

    // Show the note's live document on the spread; the place read last time
    // (this session) comes back. The same note again — the pages only claim
    // the document anew.
    void showNote(std::shared_ptr<ZNote> note);
    // Remember the place and give the document back (before the editor
    // installs another note).
    void clear();
    bool showing() const { return note_ != nullptr; }
    std::shared_ptr<ZNote> note() const { return note_; }

    // The page the person last clicked (the left one to begin with): it holds
    // the selection, takes the focus and is the search target.
    BookPage& activePage();
    int pagesShown() const { return shown_; }
    BookPage& page(int index) { return *pages_[size_t(index)]; }

    // Turn spreads: +1 forward, -1 back; the start and the end of the book.
    void pageStep(int delta);
    void jump(bool toEnd);
    // Show the spread that holds the line.
    void showLine(const PageStart& line);
    void showBlock(int block);
    // Index of the left page (0-based) and the count (-1 while being counted).
    int currentPage() const { return first_; }
    int pageCount() const { return pages_.empty() ? -1 : table_.count(); }

    // The lead page claims the document again (another mode used it) and the
    // spread is shown afresh at the same place.
    void reclaim();
    // The look changed or the document was rebuilt in place: table anew.
    void refreshAppearance();
    // One zoom for all pages (ZoomTarget::Book).
    void applyZoom(qreal zoom);
    qreal zoom() const;
    void clearMatches();

    // Where the left page starts — for the app state.
    PageStart place() const { return anchor_; }
    void restorePlace(const PageStart& place);

signals:
    // What the status line says: the chapter above the left page, the page
    // number (1-based), the count (-1 = not yet known), the percent read.
    void positionChanged(const QString& chapter, int page, int count, int percent);
    void zoomStepRequested(int delta);

protected:
    void resizeEvent(QResizeEvent* event) override;

private:
    // How many pages fit; lay them out; then show the spread at the anchor.
    void relayoutPages();
    void showSpread(int first);
    void resetTable();
    void announce();
    qreal charUnit() const;

    std::shared_ptr<ZNote> note_;
    std::vector<BookPage*> pages_;
    BookPages table_;
    int shown_ = 1;
    int first_ = 0;
    int active_ = 0;
    PageStart anchor_;
    // Places read this session, by note id (the app state takes over in §5).
    QHash<QString, PageStart> places_;
    QTimer edited_;
    QMetaObject::Connection contents_;
};

}  // namespace zametti

#endif  // ZAMETTI_ZBOOK_VIEW_H
