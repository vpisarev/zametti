// The note page of the phone shell: the reading view and a back button.
//
// THE RIG IS THE DESKTOP'S, ONE TO ONE (tests/zbook_view_test.cpp): a hidden
// NoteEditor owns the note — opening, the canon, the journal, the caret map —
// and ZBookView shows it as pages; ReadingController is the hinge between the
// two. Nothing of that is reimplemented here: the point of step 0 is to run
// the real path on the phone, not a distillate of it. On the phone every note
// is read as a book (there is no editing yet), so after the editor installs a
// note the controller is simply asked to enter the reading mode.
//
// TOUCH. The reading view knows keys and wheels only (the owner's decision in
// app/note_view.cpp: no QScroller yet). The widget is not changed; this page
// wraps it: an event filter on the pages' viewports turns a tap in the right
// third into the next page, in the left third into the previous one, and
// leaves the middle to the view (links, selection). The same filter sees the
// first paint after an open or a flip and logs it as a [perf] mark.
#ifndef ZAMETTI_ANDROID_NOTE_PAGE_H
#define ZAMETTI_ANDROID_NOTE_PAGE_H

#include "editor_widget.h"
#include "reading_controller.h"
#include "zbook_view.h"

#include <QElapsedTimer>
#include <QPoint>
#include <QWidget>

#include <memory>

class QLabel;
class QStackedWidget;
class QToolButton;

namespace zametti {

class ZStorage;

class NotePage : public QWidget {
    Q_OBJECT

public:
    explicit NotePage(QWidget* parent = nullptr);

    void setStorage(std::shared_ptr<ZStorage> storage);
    // Open the note file and show it as pages; false — the editor refused.
    bool open(const QString& file);
    // Leave the note: the pages give the document back and remember the place.
    void close();
    bool isOpen() const { return !editor_->filePath().isEmpty(); }
    // Before the process may die (background, quit): the place into the state.
    void rememberPlace();
    void refreshAppearance();

signals:
    void backRequested();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void flip(int delta);
    // A paint of the pages is awaited after `what` (open, flip): the next
    // QEvent::Paint on a page viewport closes the measurement.
    void awaitPaint(const char* what);

    QToolButton* back_;
    QLabel* title_;
    QStackedWidget* stack_;
    NoteEditor* editor_;
    ZBookView* book_;
    ReadingController reading_;
    QPoint pressedAt_;
    bool pressed_ = false;
    QElapsedTimer clock_;
    const char* awaited_ = nullptr;
};

}  // namespace zametti

#endif  // ZAMETTI_ANDROID_NOTE_PAGE_H
