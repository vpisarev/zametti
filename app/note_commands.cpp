#include "note_commands.h"

#include "image_insert.h"
#include "settings.h"
#include "zapp.h"

namespace zametti {

NoteCommands::NoteCommands(NotePanels& panels, NoteEditor& editor)
    : panels_(panels), editor_(editor) {}

void NoteCommands::settleAfter(const QString& fallback, bool wasOpen) {
    NoteTreeModel& model = panels_.model();
    panels_.selectNote(fallback);
    if (!wasOpen) return;
    QString open = fallback;
    if (open.isEmpty()) open = model.pathOfId(model.firstNoteId());
    if (!open.isEmpty()) {
        editor_.openFile(open);
        return;
    }
    // Nothing left to show: the editor must not keep a note whose file is
    // gone — a later save would resurrect it.
    editor_.closeFile();
}

void NoteCommands::removeForGood(const QString& noteId, bool wasOpen, const QString& fallback) {
    ZApp& zapp = ZApp::instance();
    const std::shared_ptr<ZStorage> store = panels_.model().storage();
    if (store == nullptr) return;
    // An open note is saved first: otherwise the last snapshot in the history
    // would be the state before the latest edits, and a person deletes what
    // they see. The removal itself is the store's business: the tombstone, or
    // the journal together with an archived note, the pictures after it.
    if (wasOpen) editor_.save(false);
    QString deleteError;
    if (!store->remove(noteId, deletedImageLimitsFrom(settings().images()), &deleteError)) {
        zapp.warn(editor_.window(), deleteError);
        return;
    }
    settleAfter(fallback, wasOpen);
}

void NoteCommands::deleteNote(const QString& noteId) {
    NoteTreeModel& model = panels_.model();
    if (!model.isStore() || noteId.isEmpty()) return;
    // The "Archive" row itself is subject to nothing: there is no file behind
    // it. The same for virtual folders: hasNote answers about the catalogue,
    // and they are not in it.
    if (!model.hasNote(noteId)) return;
    const std::shared_ptr<ZStorage> store = model.storage();
    if (store == nullptr) return;
    ZApp& zapp = ZApp::instance();
    const QString file = model.pathOfId(noteId);
    const bool wasOpen = file == editor_.filePath();

    // The neighbour in the folder is the next selected one.
    const QString fallback = model.pathOfId(model.neighbourOf(noteId));

    // Empty — to the OS trash without a word. An empty folder has no children;
    // an empty note has no meaningful text: the open one is measured by the
    // document (what was typed may not be saved yet), a closed one by the
    // store.
    const bool empty = wasOpen && !model.isFolderId(noteId)
                           ? editor_.toPlainText().trimmed().isEmpty()
                           : store->isEmptyNote(noteId);
    if (empty || model.inArchiveId(noteId)) {
        if (empty) {
            removeForGood(noteId, wasOpen, fallback);
            return;
        }
        // The only question about a deletion in the program (the owner's
        // exception to the "no dialogs" rule); through the same door and in
        // the same look as the history question. The answer comes as a signal
        // and the removal happens in it — so the continuation carries VALUES:
        // by the time the person answers, this frame is long gone.
        zapp.ask(editor_.window(),
                 QStringLiteral("Delete \"%1\" permanently?").arg(model.titleOfId(noteId)),
                 [this, noteId, wasOpen, fallback](bool yes) {
                     if (yes) removeForGood(noteId, wasOpen, fallback);
                 });
        return;
    }

    // ARCHIVING. No move: `parent` stays as it was, the header gets the mark,
    // and the body goes into the journal — the file becomes a stub
    // (store/archive.h). An open note is saved first: the person puts away
    // what they see, and the latest edits must reach the history before the
    // cut.
    if (wasOpen) editor_.save(false);
    // A folder leaves with its contents — the store knows that.
    QStringList failed;
    store->archive(noteId, NoteEditor::historyRules(), &failed);
    if (!failed.isEmpty())
        zapp.warn(editor_.window(), QStringLiteral("Not everything could be archived:\n%1")
                                        .arg(failed.join(QLatin1Char('\n'))));
    // The open note is re-read from disk: a stub stands in its place now, and
    // the editor must show what the file holds, not what it remembers.
    if (wasOpen) editor_.openFile(file);
    settleAfter(fallback, wasOpen);
}

}  // namespace zametti
