#include "reading_controller.h"

namespace zametti {

ReadingController::ReadingController(NoteEditor& editor, ZBookView& view, QObject* parent)
    : QObject(parent), editor_(editor), view_(view) {
    // THE LEAVING VIEW DETACHES FIRST: before the editor installs the next
    // note, the pages give the old document back (and remember the place).
    connect(&editor_, &NoteEditor::fileAboutToChange, this, [this](const QString&) {
        if (active_) view_.clear();
    });
    connect(&editor_, &NoteEditor::fileChanged, this, [this](const QString&) { refill(); });
}

bool ReadingController::enter() {
    if (active_) return true;
    if (editor_.filePath().isEmpty()) return false;
    // A hidden editor chasing its caret on every layout step would lay the
    // whole book out for nothing.
    editor_.releaseCaret();
    view_.showNote(editor_.noteHandle());
    active_ = true;
    emit modeChanged(true);
    return true;
}

void ReadingController::leave() {
    if (!active_) return;
    view_.clear();
    editor_.takeOverDocument();
    active_ = false;
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
    if (editor_.isBookNote()) {
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
