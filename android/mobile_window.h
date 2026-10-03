// The window of the phone shell: a stack of two pages — the list and the
// note — and the back key. QMainWindow has nothing to offer here (no menu
// bar, no dock), so a plain widget with a QStackedWidget.
//
// BACK (brief §8): on the note → back to the list; on the list → the event is
// left alone, and Qt's default sends the activity to the background. The
// folder dialog handles Key_Back itself.
#ifndef ZAMETTI_ANDROID_MOBILE_WINDOW_H
#define ZAMETTI_ANDROID_MOBILE_WINDOW_H

#include "note_panels.h"

#include <QWidget>

#include <memory>

class QStackedWidget;

namespace zametti {

class NoteListPage;
class NotePage;
class ZStorage;

class MobileWindow : public QWidget {
    Q_OBJECT

public:
    explicit MobileWindow(std::shared_ptr<ZStorage> storage, const QString& expectedRoot,
                          QWidget* parent = nullptr);

    void openNote(const QString& file);
    void showList();
    // Before the process may die: the reading place and mode into the state.
    void rememberPlaces();
    void refreshAppearance();

protected:
    void keyPressEvent(QKeyEvent* event) override;

private:
    NotePanels panels_;
    QStackedWidget* stack_;
    NoteListPage* list_;
    NotePage* note_;
    bool choosingFolder_ = false;
};

}  // namespace zametti

#endif  // ZAMETTI_ANDROID_MOBILE_WINDOW_H
