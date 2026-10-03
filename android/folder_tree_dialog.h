// The folder tree of the phone shell: a full-screen modal dialog over the
// list. The tree is the desktop's own NoteTreeView, moved into the dialog for
// its lifetime and handed back afterwards — there is exactly one tree, and
// the primary selection → list contents wiring stays inside NotePanels.
// A tap on a folder closes the dialog (the list has already been refilled
// by the panels); "All notes" is the model's root — an invalid current index.
#ifndef ZAMETTI_ANDROID_FOLDER_TREE_DIALOG_H
#define ZAMETTI_ANDROID_FOLDER_TREE_DIALOG_H

#include <QDialog>

class QVBoxLayout;

namespace zametti {

class NotePanels;

class FolderTreeDialog : public QDialog {
    Q_OBJECT

public:
    explicit FolderTreeDialog(NotePanels& panels, QWidget* parent = nullptr);
    ~FolderTreeDialog() override;

protected:
    void keyPressEvent(QKeyEvent* event) override;

private:
    NotePanels& panels_;
    QWidget* treeHome_;   // where the tree lived before the dialog took it
    QVBoxLayout* layout_;
};

}  // namespace zametti

#endif  // ZAMETTI_ANDROID_FOLDER_TREE_DIALOG_H
