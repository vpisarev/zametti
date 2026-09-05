// ReadingController — the reading mode of the window (brief 18).
//
// The mode belongs to the application, like the source mode: it is a page of
// the text stack that shows the OPEN note's live document as a book (ZBookView)
// while the editor keeps owning the note — opening, saving, the caret map.
// A note with role: book opens in the reading mode by itself; any other note
// can be read too. Switching never rebuilds the document: the view that comes
// on screen claims it (NoteView::takeOverDocument), the other lets go.
#ifndef ZAMETTI_READING_CONTROLLER_H
#define ZAMETTI_READING_CONTROLLER_H

#include "editor_widget.h"
#include "zbook_view.h"

#include <QObject>

namespace zametti {

class ReadingController : public QObject {
    Q_OBJECT

public:
    ReadingController(NoteEditor& editor, ZBookView& view, QObject* parent = nullptr);

    // false — no note is open, nothing to read.
    bool enter();
    void leave();
    bool toggle();
    bool active() const { return active_; }

    // The note changed (after the editor installed it): a book is read, an
    // ordinary note goes back to the editor.
    void refill();
    void refreshAppearance();

signals:
    void modeChanged(bool on);

private:
    NoteEditor& editor_;
    ZBookView& view_;
    bool active_ = false;
};

}  // namespace zametti

#endif  // ZAMETTI_READING_CONTROLLER_H
