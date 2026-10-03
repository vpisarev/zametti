#include "note_page.h"

#include "perf_log.h"
#include "zapp.h"
#include "zstorage.h"

#include <QAbstractScrollArea>
#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QStackedWidget>
#include <QToolButton>
#include <QVBoxLayout>

namespace zametti {

namespace {
// 48 dp: the smallest touch target Android recommends. Qt on Android sets the
// device pixel ratio from the screen density, so a logical pixel is a dp.
constexpr int kTouchTarget = 48;
// A press that travelled further than this is a drag (selection, a future
// swipe), not a tap.
constexpr int kTapSlack = 12;
}  // namespace

NotePage::NotePage(QWidget* parent)
    : QWidget(parent),
      back_(new QToolButton(this)),
      title_(new QLabel(this)),
      stack_(new QStackedWidget(this)),
      editor_(new NoteEditor(stack_)),
      book_(new ZBookView(stack_)),
      reading_(*editor_, *book_, this) {
    back_->setText(QStringLiteral("←"));
    back_->setToolTip(QStringLiteral("Back"));
    back_->setMinimumSize(kTouchTarget, kTouchTarget);
    back_->setAutoRaise(true);
    connect(back_, &QToolButton::clicked, this, &NotePage::backRequested);
    title_->setTextFormat(Qt::PlainText);

    auto* bar = new QHBoxLayout;
    bar->setContentsMargins(4, 0, 4, 0);
    bar->addWidget(back_);
    bar->addWidget(title_, 1);

    // The editor is a page of the stack too, exactly as on the desktop, but
    // the stack never shows it: the controller's modeChanged would switch to
    // it for an ordinary note, and the phone overrides that (see open()).
    stack_->addWidget(editor_);
    stack_->addWidget(book_);
    stack_->setCurrentWidget(book_);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addLayout(bar);
    layout->addWidget(stack_, 1);

    // The pages exist from the view's constructor on; the filter lives on
    // their viewports because that is where the mouse (and the synthesised
    // touch) arrives and where the paint happens.
    for (QAbstractScrollArea* page : book_->findChildren<QAbstractScrollArea*>())
        page->viewport()->installEventFilter(this);

    connect(book_, &ZBookView::positionChanged, this,
            [this](const QString& chapter, int page, int count, int) {
                const QString note = editor_->noteHandle() ? editor_->noteHandle()->title()
                                                           : QString();
                QString text = chapter.isEmpty() ? note : chapter;
                if (count > 0)
                    text += QStringLiteral("  ·  %1/%2").arg(page).arg(count);
                else if (page > 0)
                    text += QStringLiteral("  ·  %1").arg(page);
                title_->setText(text);
            });
}

void NotePage::setStorage(std::shared_ptr<ZStorage> storage) {
    editor_->setStorage(std::move(storage));
}

bool NotePage::open(const QString& file) {
    clock_.restart();
    awaitPaint("open");
    // The editor installs the note and emits fileChanged; the controller's
    // refill() then enters the reading mode for a book and leaves it for a
    // note — and the phone reads everything, so enter() is asked once more.
    if (!editor_->openFile(file, /*takeFocus=*/false)) {
        awaited_ = nullptr;
        return false;
    }
    if (!reading_.active()) reading_.enter();
    stack_->setCurrentWidget(book_);
    perfLog("open: note installed", clock_.elapsed());
    return true;
}

void NotePage::close() {
    if (!isOpen()) return;
    // leave() gives the document back to the editor and remembers the place
    // of the reading half; the editor keeps the note so that the next open
    // of the same file is a cache hit.
    reading_.leave();
}

void NotePage::rememberPlace() {
    if (reading_.active()) book_->rememberPlace();
    reading_.rememberMode();
}

void NotePage::refreshAppearance() {
    editor_->refreshAppearance();
    reading_.refreshAppearance();
}

void NotePage::flip(int delta) {
    clock_.restart();
    awaitPaint(delta > 0 ? "flip forward" : "flip back");
    book_->pageStep(delta);
}

void NotePage::awaitPaint(const char* what) { awaited_ = what; }

bool NotePage::eventFilter(QObject* watched, QEvent* event) {
    switch (event->type()) {
    case QEvent::Paint:
        if (awaited_ != nullptr) {
            perfLog(QByteArray(awaited_) + " → paint", clock_.elapsed());
            awaited_ = nullptr;
        }
        return false;
    case QEvent::MouseButtonPress: {
        auto* mouse = static_cast<QMouseEvent*>(event);
        if (mouse->button() != Qt::LeftButton) return false;
        pressed_ = true;
        pressedAt_ = mouse->pos();
        return false;
    }
    case QEvent::MouseButtonRelease: {
        auto* mouse = static_cast<QMouseEvent*>(event);
        if (mouse->button() != Qt::LeftButton || !pressed_) return false;
        pressed_ = false;
        if ((mouse->pos() - pressedAt_).manhattanLength() > kTapSlack) return false;
        auto* viewport = qobject_cast<QWidget*>(watched);
        if (viewport == nullptr) return false;
        // Thirds of the WHOLE view, not of one page: with two pages on a
        // wide screen the gutter must not count as "left edge of the right
        // page".
        const int x = book_->mapFromGlobal(mouse->globalPosition().toPoint()).x();
        const int width = book_->width();
        if (x < width / 3) {
            flip(-1);
            return true;
        }
        if (x > width * 2 / 3) {
            flip(+1);
            return true;
        }
        return false;
    }
    default:
        return false;
    }
}

}  // namespace zametti
