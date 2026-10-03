#include "mobile_window.h"

#include "folder_tree_dialog.h"
#include "note_list_page.h"
#include "note_page.h"
#include "note_tree.h"
#include "zstorage.h"

#include <QItemSelectionModel>
#include <QKeyEvent>
#include <QLayout>
#include <QShowEvent>
#include <QWindow>
#include <QStackedWidget>
#include <QVBoxLayout>

namespace zametti {

MobileWindow::MobileWindow(std::shared_ptr<ZStorage> storage, const QString& expectedRoot,
                           QWidget* parent)
    : QWidget(parent),
      panels_(storage, this),
      stack_(new QStackedWidget(this)),
      list_(new NoteListPage(panels_, stack_)),
      note_(new NotePage(stack_)) {
    setWindowTitle(QStringLiteral("zametti"));
    note_->setStorage(storage);

    stack_->addWidget(list_);
    stack_->addWidget(note_);
    stack_->setCurrentWidget(list_);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(stack_);

    // The panels are switched on first — that shows both widgets — and only
    // then the tree is put away: it is on screen inside the folder dialog and
    // nowhere else, while the panels keep owning it.
    panels_.setVisible(true);
    panels_.tree().setParent(this);
    panels_.tree().setVisible(false);

    // EDGE TO EDGE. From Android 15 on (targetSdk 35+) the window is drawn
    // under the status bar and the navigation bar; Qt reports what they cover
    // as the window's safe-area margins, and the layout keeps out of them.
    // The margins are known only once the window has a native handle, hence
    // the showEvent hook below.

    if (storage == nullptr || !panels_.isStore()) list_->showNoStore(expectedRoot);

    connect(list_, &NoteListPage::folderRequested, this, [this] {
        choosingFolder_ = true;
        FolderTreeDialog dialog(panels_, this);
        dialog.showFullScreen();
        dialog.exec();
        choosingFolder_ = false;
    });
    // NOT noteChosen. The panels announce a note whenever the list is
    // refilled — a folder picked, the tree rebuilt after the catalogue
    // caught up — because on the desktop the editor shows the first note at
    // once. On the phone the list is the screen, and a note opens on a tap
    // only: the list page reports the tapped row itself.
    connect(list_, &NoteListPage::noteTapped, this, &MobileWindow::openNote);
    connect(note_, &NotePage::backRequested, this, &MobileWindow::showList);

    // THE FIRST LIST. On the desktop the list fills when the last open note
    // is shown (showNote(primary)); the phone opens nothing at start, so the
    // primary selection is announced once by hand: the current folder of the
    // tree (its root row when nothing is current) is re-set, and the panels
    // fill the list by it. The first note they announce is not opened.
    if (panels_.isStore()) {
        choosingFolder_ = true;
        QItemSelectionModel* selection = panels_.tree().selectionModel();
        QModelIndex folder = selection->currentIndex();
        if (!folder.isValid()) folder = panels_.model().index(0, 0, QModelIndex());
        selection->setCurrentIndex(QModelIndex(), QItemSelectionModel::NoUpdate);
        selection->setCurrentIndex(folder, QItemSelectionModel::ClearAndSelect);
        choosingFolder_ = false;
    }
}

void MobileWindow::openNote(const QString& file) {
    if (file.isEmpty()) return;
    // THE PAGE GETS ITS SIZE FIRST. A hidden page of the stack is never laid
    // out (seen: 100×30 at the moment of open), the document would be built
    // and paginated for that width, and the later resize does not reach every
    // layer — the right edge of the Karamazovs came out clipped and the page
    // count was 1233 instead of 1332. So: show the page, give it the stack's
    // geometry by hand (the stacked layout would do it on the next event),
    // and only then install the note.
    stack_->setCurrentWidget(note_);
    note_->setGeometry(stack_->contentsRect());
    if (!note_->open(file)) {
        stack_->setCurrentWidget(list_);
        return;
    }
    panels_.setCurrentNote(file);
    panels_.showNote(file);
}

void MobileWindow::showList() {
    note_->close();
    stack_->setCurrentWidget(list_);
}

void MobileWindow::rememberPlaces() { note_->rememberPlace(); }

void MobileWindow::refreshAppearance() {
    list_->refreshAppearance();
    note_->refreshAppearance();
}

void MobileWindow::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    QWindow* handle = windowHandle();
    if (handle == nullptr || safeAreaWired_) return;
    safeAreaWired_ = true;
    const auto apply = [this, handle] {
        const QMargins m = handle->safeAreaMargins();
        layout()->setContentsMargins(m);
    };
    connect(handle, &QWindow::safeAreaMarginsChanged, this, [apply](const QMargins&) { apply(); });
    apply();
}

void MobileWindow::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Back || event->key() == Qt::Key_Escape) {
        if (stack_->currentWidget() == note_) {
            showList();
            event->accept();
            return;
        }
        // On the list: not ours — Qt's default puts the activity away.
        event->ignore();
        return;
    }
    QWidget::keyPressEvent(event);
}

}  // namespace zametti
