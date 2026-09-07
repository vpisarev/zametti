// NoteCommands: the window's deletion flow, driven the way the window drives it.
//
// The regression (04.09.2026 → 07.09.2026): "Delete permanently?" is asked
// through ZApp::ask, which opens the box and returns at once; the continuation
// of the old main.cpp lambda referenced a closure on the stack frame of the
// lambda that had already returned. The note was removed, then the program
// crashed. The suite answers the real box, and scrubs the stack before
// answering — with a dangling reference the continuation reads garbage and
// the process falls, deterministically, instead of passing by luck.

#include "editor_widget.h"
#include "note_commands.h"
#include "note_panels.h"
#include "test_util.h"
#include "zstorage.h"

#include <QApplication>
#include <QCoreApplication>
#include <QFile>
#include <QMessageBox>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTest>

#include <cstddef>
#include <cstdio>
#include <string>

using zametti::NoteCommands;
using zametti::NotePanels;
using zametti::ZStorage;

namespace {

zametti::ZJournal::Rules rules() { return zametti::ZJournal::Rules{}; }
std::string s(const QString& text) { return text.toStdString(); }

volatile char g_sink = 0;

// Overwrites a good deal of the stack below the caller. A continuation that
// still points into a returned frame reads this pattern instead of the closure
// it expects: the old bug crashes here on every build, not only under ASan.
Q_NEVER_INLINE void scrubStack() {
    volatile char junk[256 * 1024];
    for (std::size_t i = 0; i < sizeof junk; i += 16) junk[i] = char(0xA5);
    g_sink = junk[sizeof junk - 16];
}

// The window: panels, the editor and the wiring between them, as in main().
struct Rig {
    std::shared_ptr<ZStorage> storage;
    NotePanels panels;
    zametti::NoteEditor editor;
    NoteCommands commands;

    explicit Rig(std::shared_ptr<ZStorage> store)
        : storage(std::move(store)), panels(storage), commands(panels, editor) {
        editor.setStorage(storage);
        editor.setAttribute(Qt::WA_DontShowOnScreen);
        editor.resize(600, 400);
        editor.show();
        panels.tree().setAttribute(Qt::WA_DontShowOnScreen);
        panels.tree().resize(300, 400);
        panels.tree().show();
        panels.listView().setAttribute(Qt::WA_DontShowOnScreen);
        panels.listView().resize(300, 400);
        panels.listView().show();
        QObject::connect(&editor, &zametti::NoteEditor::fileChanged, &panels,
                         &NotePanels::setCurrentNote);
        QObject::connect(
            &editor, &zametti::NoteEditor::fileChanged, &panels,
            [this](const QString& file) { panels.showNote(file); }, Qt::QueuedConnection);
        QObject::connect(&panels, &NotePanels::noteChosen, &editor,
                         [this](const QString& file, bool takeFocus) {
                             editor.openFile(file, takeFocus);
                         });
        QObject::connect(&editor, &zametti::NoteEditor::fileSaved, &panels,
                         [this](const QString& file) { panels.model().refreshNote(file); });
    }

    void settle() {
        QCoreApplication::processEvents();
        QCoreApplication::processEvents();
    }

    // The visible question, or null.
    QMessageBox* question() {
        QMessageBox* box = nullptr;
        for (QMessageBox* candidate : editor.window()->findChildren<QMessageBox*>())
            if (candidate->isVisible()) box = candidate;
        return box;
    }

    // Answers the visible question with its own button; false — no question.
    bool answer(bool yes) {
        QMessageBox* box = question();
        if (box == nullptr) return false;
        QPushButton* yesButton = nullptr;
        QPushButton* noButton = nullptr;
        for (QAbstractButton* candidate : box->buttons()) {
            if (box->buttonRole(candidate) == QMessageBox::YesRole)
                yesButton = qobject_cast<QPushButton*>(candidate);
            if (box->buttonRole(candidate) == QMessageBox::NoRole)
                noButton = qobject_cast<QPushButton*>(candidate);
        }
        if (yesButton == nullptr || noButton == nullptr) return false;
        (yes ? yesButton : noButton)->click();
        QTest::qWait(30);
        settle();
        return true;
    }
};

// A store with a folder holding `keep`, and two archived title-only notes —
// the owner's case: an archived note with nothing but a heading.
struct Store {
    QTemporaryDir home;
    std::shared_ptr<ZStorage> storage;
    QString folder, keep, victim, other;

    bool make() {
        const QString root = home.path() + QStringLiteral("/store");
        QString error;
        if (!ZStorage(root).init(&error)) return false;
        storage = std::make_shared<ZStorage>(root);
        storage->reload();
        folder = storage->createNote(QString(), true, &error);
        if (!storage->rename(folder, QStringLiteral("Папка"), rules(), &error)) return false;
        keep = storage->createNote(folder, false, &error);
        victim = storage->createNote(folder, false, &error);
        other = storage->createNote(folder, false, &error);
        if (!storage->rename(keep, QStringLiteral("Остаётся"), rules(), &error) ||
            !storage->rename(victim, QStringLiteral("Only a title"), rules(), &error) ||
            !storage->rename(other, QStringLiteral("Second archived"), rules(), &error))
            return false;
        QStringList failed;
        if (!storage->archive(victim, rules(), &failed) || !failed.isEmpty()) return false;
        if (!storage->archive(other, rules(), &failed) || !failed.isEmpty()) return false;
        return true;
    }
};

int checkDeletePermanently() {
    Store store;
    ZT_TRUE("store with two archived title-only notes", store.make());
    Rig rig(store.storage);
    zametti::NoteTreeModel& model = rig.panels.model();
    const QString victimPath = store.storage->pathOf(store.victim);
    const QString otherPath = store.storage->pathOf(store.other);

    // The owner's case: the archived note is the one shown, and it is not
    // "empty" — a heading is a block. So the question is asked, not skipped.
    ZT_TRUE("victim is in the archive", model.inArchiveId(store.victim));
    ZT_TRUE("opened in the editor", rig.editor.openFile(victimPath));
    rig.settle();
    // As at the window's start: the tree cursor is the folder of the open
    // note — the Archive — so the list shows the archived notes.
    rig.panels.showNote(victimPath, /*primary=*/true);
    rig.settle();
    ZT_TRUE("a title-only note is not empty for the store", !store.storage->isEmptyNote(store.victim));
    ZT_TRUE("nor for the editor", !rig.editor.toPlainText().trimmed().isEmpty());

    // The question is shown and the command returns at once; nothing happened yet.
    rig.commands.deleteNote(store.victim);
    QTest::qWait(30);
    QMessageBox* box = rig.question();
    ZT_TRUE("the question is asked", box != nullptr);
    if (box == nullptr) return zt::report("delete permanently");
    ZT_EQ("the question names the note", "Delete \"Only a title\" permanently?", s(box->text()));
    ZT_TRUE("the file is still there before the answer", QFile::exists(victimPath));
    QPushButton* noButton = nullptr;
    for (QAbstractButton* candidate : box->buttons())
        if (box->buttonRole(candidate) == QMessageBox::NoRole)
            noButton = qobject_cast<QPushButton*>(candidate);
    ZT_TRUE("«No» is the default", noButton != nullptr && box->defaultButton() == noButton);

    // "No": everything stays.
    ZT_TRUE("answered no", rig.answer(false));
    ZT_TRUE("file kept after no", QFile::exists(victimPath));
    ZT_TRUE("catalogue kept after no", model.hasNote(store.victim));
    ZT_EQ("editor kept after no", s(victimPath), s(rig.editor.filePath()));

    // "Yes" — after the frame of deleteNote() is gone and the stack below us is
    // scrubbed. The neighbour in the archive takes the selection and the editor.
    rig.commands.deleteNote(store.victim);
    QTest::qWait(30);
    scrubStack();
    ZT_TRUE("answered yes", rig.answer(true));
    ZT_TRUE("file removed", !QFile::exists(victimPath));
    ZT_TRUE("catalogue without the note", !model.hasNote(store.victim));
    ZT_TRUE("the other archived note survived", model.hasNote(store.other));
    ZT_EQ("editor moved to the neighbour", s(otherPath), s(rig.editor.filePath()));
    ZT_EQ("panels follow the editor", s(otherPath), s(rig.panels.currentNote()));
    ZT_EQ("list row is the neighbour", s(otherPath),
          s(rig.panels.list().pathAt(rig.panels.listView().currentIndex())));

    // The last archived note: nothing to fall back to in the archive — the
    // first note of the store is shown; the editor never keeps a dead path.
    rig.commands.deleteNote(store.other);
    QTest::qWait(30);
    scrubStack();
    ZT_TRUE("answered yes for the last one", rig.answer(true));
    ZT_TRUE("last archived removed", !QFile::exists(otherPath));
    ZT_TRUE("editor left the removed note", rig.editor.filePath() != otherPath);
    ZT_TRUE("no question is pending", rig.question() == nullptr);
    return zt::report("delete permanently");
}

// The synchronous branch: an empty note goes without a question, and a live
// one is archived, not asked about.
int checkEmptyAndArchive() {
    Store store;
    ZT_TRUE("store", store.make());
    Rig rig(store.storage);
    zametti::NoteTreeModel& model = rig.panels.model();
    QString error;
    const QString blank = store.storage->createNote(store.folder, false, &error);
    const QString blankPath = store.storage->pathOf(blank);
    QCoreApplication::processEvents();
    ZT_TRUE("a fresh note is empty", store.storage->isEmptyNote(blank));
    rig.commands.deleteNote(blank);
    QTest::qWait(30);
    ZT_TRUE("no question for an empty note", rig.question() == nullptr);
    ZT_TRUE("empty note gone at once", !QFile::exists(blankPath));
    ZT_TRUE("and out of the catalogue", !model.hasNote(blank));

    // A live note with a title is archived: the file stays as a stub.
    const QString keepPath = store.storage->pathOf(store.keep);
    ZT_TRUE("open keep", rig.editor.openFile(keepPath));
    rig.settle();
    rig.commands.deleteNote(store.keep);
    QTest::qWait(30);
    rig.settle();
    ZT_TRUE("no question when archiving", rig.question() == nullptr);
    ZT_TRUE("archived", model.inArchiveId(store.keep));
    ZT_TRUE("the stub is on disk", QFile::exists(keepPath));
    return zt::report("empty and archive");
}

int ztRunSuite() {
    int failures = 0;
    failures += checkDeletePermanently();
    failures += checkEmptyAndArchive();
    return failures;
}

}  // namespace

TEST(NoteCommands, All) { EXPECT_EQ(0, ztRunSuite()); }
