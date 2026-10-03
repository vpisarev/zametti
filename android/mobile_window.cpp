#include "mobile_window.h"

#include "folder_tree_dialog.h"
#include "note_list_page.h"
#include "note_page.h"
#include "zstorage.h"

#include <QKeyEvent>
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

    // The tree is not on screen outside the dialog; the panels own it.
    panels_.tree().setParent(this);
    panels_.tree().setVisible(false);
    panels_.setVisible(true);

    if (storage == nullptr || !panels_.isStore()) list_->showNoStore(expectedRoot);

    connect(list_, &NoteListPage::folderRequested, this, [this] {
        choosingFolder_ = true;
        FolderTreeDialog dialog(panels_, this);
        dialog.showFullScreen();
        dialog.exec();
        choosingFolder_ = false;
    });
    // Picking a folder makes the panels select the first note and announce
    // it (desktop behaviour: the editor shows it). On the phone the list is
    // the screen; a note opens on a tap only.
    connect(&panels_, &NotePanels::noteChosen, this, [this](const QString& file, bool) {
        if (choosingFolder_) return;
        if (stack_->currentWidget() != list_) return;
        openNote(file);
    });
    connect(list_, &NoteListPage::noteTapped, this, &MobileWindow::openNote);
    connect(note_, &NotePage::backRequested, this, &MobileWindow::showList);
}

void MobileWindow::openNote(const QString& file) {
    if (file.isEmpty()) return;
    if (!note_->open(file)) return;
    panels_.setCurrentNote(file);
    panels_.showNote(file);
    stack_->setCurrentWidget(note_);
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
