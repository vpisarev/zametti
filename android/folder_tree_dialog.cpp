#include "folder_tree_dialog.h"

#include "note_panels.h"
#include "note_tree.h"

#include <QKeyEvent>
#include <QPushButton>
#include <QVBoxLayout>

namespace zametti {

FolderTreeDialog::FolderTreeDialog(NotePanels& panels, QWidget* parent)
    : QDialog(parent), panels_(panels), treeHome_(panels.tree().parentWidget()),
      layout_(new QVBoxLayout(this)) {
    setWindowTitle(QStringLiteral("Folders"));
    setModal(true);

    auto* all = new QPushButton(QStringLiteral("All notes"), this);
    all->setMinimumHeight(48);
    connect(all, &QPushButton::clicked, this, [this] {
        // The root: no current folder. currentChanged → folderPicked → the
        // whole store in the list.
        panels_.tree().selectionModel()->setCurrentIndex(QModelIndex(),
                                                          QItemSelectionModel::ClearAndSelect);
        accept();
    });

    NoteTreeView& tree = panels_.tree();
    layout_->setContentsMargins(8, 8, 8, 8);
    layout_->addWidget(all);
    layout_->addWidget(&tree, 1);
    tree.setVisible(true);
    tree.expandAll();
    // Any tap on a row — even the current one — is the choice; the panels
    // refill the list on clicked() as well, so the dialog only has to go.
    connect(&tree, &QAbstractItemView::clicked, this, [this](const QModelIndex&) { accept(); });

    auto* cancel = new QPushButton(QStringLiteral("Cancel"), this);
    cancel->setMinimumHeight(48);
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    layout_->addWidget(cancel);
}

FolderTreeDialog::~FolderTreeDialog() {
    // The tree goes home: NotePanels owns it and the next dialog takes it again.
    NoteTreeView& tree = panels_.tree();
    layout_->removeWidget(&tree);
    tree.setParent(treeHome_);
    tree.setVisible(false);
}

void FolderTreeDialog::keyPressEvent(QKeyEvent* event) {
    // The phone's back gesture/button arrives as Key_Back; QDialog knows
    // Escape only.
    if (event->key() == Qt::Key_Back) {
        reject();
        return;
    }
    QDialog::keyPressEvent(event);
}

}  // namespace zametti
