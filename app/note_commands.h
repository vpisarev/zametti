// Window commands over the store that need the panels AND the editor at once.
//
// A separate object, not lambdas in main(): the wiring of the window is what
// no suite could call, and it is exactly what broke silently on 04.09.2026 —
// the "Delete permanently?" question became non-blocking (ZApp::ask opens the
// box and returns), and its continuation kept a reference to a closure on the
// stack frame of the lambda that had already returned. The note was removed,
// then the program crashed on the dangling frame. Here the continuation is a
// method, and everything it needs travels BY VALUE.

#ifndef ZAMETTI_NOTE_COMMANDS_H
#define ZAMETTI_NOTE_COMMANDS_H

#include "editor_widget.h"
#include "note_panels.h"

#include <QString>

namespace zametti {

class NoteCommands {
public:
    NoteCommands(NotePanels& panels, NoteEditor& editor);

    // Del / "Archive" / "Delete permanently". An empty note or an empty folder
    // goes to the OS trash at once; a live note is archived (body into the
    // journal, the file becomes a stub); an archived one is removed for good
    // after the only confirmation in the program (the owner's exception to the
    // "no dialogs" rule). Selection settles on the neighbour afterwards.
    // Returns at once when a question is shown: the answer arrives as a signal.
    void deleteNote(const QString& noteId);

protected:
    // The continuation of the question. Runs long after deleteNote() returned:
    // only values and the collaborators that outlive the window are touched.
    void removeForGood(const QString& noteId, bool wasOpen, const QString& fallback);
    // After a removal or an archiving: the neighbour becomes selected, and
    // takes the editor if the removed note was the open one.
    void settleAfter(const QString& fallback, bool wasOpen);

    NotePanels& panels_;
    NoteEditor& editor_;
};

}  // namespace zametti

#endif
