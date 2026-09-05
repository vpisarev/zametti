// BookPage — one page of a book shown in the reading mode (brief 18).
//
// The page shows the LIVE document of the note the editor owns — the same
// QTextDocument, the same undo stack (owner's decision: reading is another
// view of the note, not another copy). What the page adds is the frame: no
// caret, no scroll bars, keys and the wheel turn pages instead of scrolling,
// the line cut at the bottom is hidden under a strip of page colour, and the
// bookmark glyph lives in the left margin. Two pages side by side (the spread,
// ZBookView) are two of these over the same document; the left one leads —
// it claims the document's object handlers and text width (takeOverDocument),
// the right one only shows.
#ifndef ZAMETTI_BOOK_PAGE_H
#define ZAMETTI_BOOK_PAGE_H

#include "book_pages.h"
#include "document.h"
#include "note_view.h"
#include "znote.h"

#include <memory>

namespace zametti {

class BookPage : public NoteView {
    Q_OBJECT

public:
    explicit BookPage(QWidget* parent = nullptr);

    // Show the note's live document. lead — this page claims the document
    // (handlers, font, width); a follower shows it as it is. The page keeps
    // the note alive: a view must hold the note, not just its document.
    void showNote(std::shared_ptr<ZNote> note, bool lead);
    // Give the document back: the page returns to its own blank document.
    // The leaving view detaches FIRST — Qt drops the paint device of the
    // layout a view leaves, so the other view must not be painting it yet.
    void clear();
    std::shared_ptr<ZNote> note() const { return note_; }
    bool lead() const { return lead_; }

    // Put the given line at the top of the page. The last page may not reach
    // its start: the scroll range ends where the document does.
    void showStart(const PageStart& start);
    const PageStart& start() const { return start_; }
    // The line under the top edge right now (after a mouse drag the view may
    // have drifted; the spread snaps it back).
    PageStart topLine() const;
    // The line that holds the document y — for the spread to turn a search hit
    // or a scroll into a page.
    PageStart lineAt(qreal y) const;
    qreal pageHeight() const;

    // The reading font for a note that is not a book (a book's document is
    // already built with it); re-read from the settings.
    void refreshAppearance();

signals:
    // +1 — the next spread, -1 — the previous one (keys, the wheel).
    void pageStepRequested(int delta);
    // Home / End.
    void jumpRequested(bool toEnd);
    // The person clicked here: this page is the active one (selection, search).
    void activated();
    // A place must come into view (a search hit): the spread shows its page.
    void revealRequested(const PageStart& line);

protected:
    QFont zoomedBaseFont(qreal zoom) const override;
    int pagePadding() const override;
    const ZDocStyle& columnStyle() const override { return *reading_; }
    NoteSearch& searchCache() override;
    const NoteSearch& searchCache() const override;
    void revealInGolden(const QRectF& place) override;
    void keyPressEvent(QKeyEvent* event) override;
    bool event(QEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

private:
    std::shared_ptr<ZNote> note_;
    // The page's own document while nothing is shown.
    ZDocument blank_;
    std::shared_ptr<const ZDocStyle> reading_;
    PageStart start_;
    bool lead_ = false;
    bool snapping_ = false;
    int wheelAccumulated_ = 0;
};

}  // namespace zametti

#endif  // ZAMETTI_BOOK_PAGE_H
