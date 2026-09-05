#include "reading_controller.h"

#include "zapp.h"

namespace zametti {

ReadingController::ReadingController(NoteEditor& editor, ZBookView& view, QObject* parent)
    : QObject(parent), editor_(editor), view_(view) {
    // THE LEAVING VIEW DETACHES FIRST: before the editor installs the next
    // note, the pages give the old document back (and remember the place).
    connect(&editor_, &NoteEditor::fileAboutToChange, this, [this](const QString&) {
        rememberMode();
        if (active_) view_.clear();
    });
    connect(&editor_, &NoteEditor::fileChanged, this, [this](const QString&) { refill(); });
    // BOOKMARKS FROM THE PAGES go through the editor — the owner of the note
    // and the store; the pages only ask and repaint.
    connect(&view_, &ZBookView::bookmarkToggleRequested, this,
            [this](int block) { editor_.toggleBookmark(block); });
    connect(&view_, &ZBookView::bookmarkStepRequested, this, [this](int direction) {
        // From the spread's first line; forward, the marks still on this
        // spread are skipped — "next" means the next one to turn to.
        int block = editor_.stepBookmark(direction, view_.place().block);
        while (block >= 0 && direction > 0 && view_.blockOnSpread(block))
            block = editor_.stepBookmark(direction, block);
        if (block >= 0) view_.showBlock(block);
    });
    connect(&editor_, &NoteEditor::bookmarksChanged, this, [this] { view_.refreshMarks(); });
}

bool ReadingController::enter() {
    if (active_) return true;
    if (editor_.filePath().isEmpty()) return false;
    // A hidden editor chasing its caret on every layout step would lay the
    // whole book out for nothing.
    editor_.releaseCaret();
    view_.showNote(editor_.noteHandle());
    active_ = true;
    rememberMode();
    emit modeChanged(true);
    return true;
}

void ReadingController::rememberMode() {
    if (editor_.filePath().isEmpty()) return;
    const std::shared_ptr<ZNote> note = editor_.noteHandle();
    if (note == nullptr) return;
    ZApp::instance().state().rememberMode(note->id(), active_ ? 1 : 2);
}

void ReadingController::leave() {
    if (!active_) return;
    view_.clear();
    editor_.takeOverDocument();
    active_ = false;
    rememberMode();
    emit modeChanged(false);
}

bool ReadingController::toggle() {
    if (active_) {
        leave();
        return false;
    }
    return enter();
}

void ReadingController::refill() {
    if (editor_.filePath().isEmpty()) {
        leave();
        return;
    }
    // THE MODE THE NOTE WAS LEFT IN wins; never chosen — a book is read, a
    // note is edited.
    const int remembered = ZApp::instance().state().caretOf(editor_.noteHandle()->id()).mode;
    const bool read = remembered == 1 || (remembered == 0 && editor_.isBookNote());
    if (read) {
        if (active_) {
            editor_.releaseCaret();
            view_.showNote(editor_.noteHandle());
        } else {
            enter();
        }
        return;
    }
    if (active_) leave();
}

void ReadingController::refreshAppearance() {
    if (active_) view_.refreshAppearance();
}

}  // namespace zametti
