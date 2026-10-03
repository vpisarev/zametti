// The root page of the phone shell: the note list of the current folder,
// with a toolbar above it — the folder button (the folder's name), the sort
// menu, and the two buttons the brief wants present and disabled (new folder,
// new note: no editing in step 0).
//
// The list is the desktop's middle column — NotePanels::listPanel() with its
// model and delegate (title, snippet, date, the book cover) — and the panels
// keep deciding what the list contains and in which order (CLAUDE.md: the
// middle column belongs to the primary selection).
#ifndef ZAMETTI_ANDROID_NOTE_LIST_PAGE_H
#define ZAMETTI_ANDROID_NOTE_LIST_PAGE_H

#include <QElapsedTimer>
#include <QWidget>

class QLabel;
class QMenu;
class QToolButton;

namespace zametti {

class NotePanels;

class NoteListPage : public QWidget {
    Q_OBJECT

public:
    explicit NoteListPage(NotePanels& panels, QWidget* parent = nullptr);

    // No store on the phone yet: the list is replaced by the seeding hint.
    void showNoStore(const QString& expectedRoot);
    void refreshAppearance();

signals:
    void folderRequested();
    // A tap on a row — also on the row that is already current (coming back
    // from the note and tapping it again).
    void noteTapped(const QString& file);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    void syncFolderButton();
    void rebuildSortMenu();
    void elideButtons();

    NotePanels& panels_;
    QToolButton* folder_;
    QToolButton* sort_;
    QMenu* sortMenu_;
    QToolButton* newFolder_;
    QToolButton* newNote_;
    QLabel* hint_;
    QString folderText_;
    QString sortText_;
    QElapsedTimer clock_;
    bool firstPaintLogged_ = false;
};

}  // namespace zametti

#endif  // ZAMETTI_ANDROID_NOTE_LIST_PAGE_H
