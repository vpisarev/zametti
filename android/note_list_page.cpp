#include "note_list_page.h"

#include "note_list.h"
#include "note_panels.h"
#include "note_tree.h"
#include "perf_log.h"
#include "sort_order.h"

#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QListView>
#include <QMenu>
#include <QScroller>
#include <QToolButton>
#include <QVBoxLayout>

namespace zametti {

namespace {
constexpr int kTouchTarget = 48;

QToolButton* toolButton(const QString& text, QWidget* parent) {
    auto* button = new QToolButton(parent);
    button->setText(text);
    button->setMinimumSize(kTouchTarget, kTouchTarget);
    button->setAutoRaise(true);
    button->setToolButtonStyle(Qt::ToolButtonTextOnly);
    return button;
}
}  // namespace

NoteListPage::NoteListPage(NotePanels& panels, QWidget* parent)
    : QWidget(parent),
      panels_(panels),
      folder_(toolButton(QStringLiteral("All notes"), this)),
      sort_(toolButton(QStringLiteral("Sort"), this)),
      sortMenu_(new QMenu(this)),
      newFolder_(toolButton(QStringLiteral("+ folder"), this)),
      newNote_(toolButton(QStringLiteral("+ note"), this)),
      hint_(new QLabel(this)) {
    clock_.start();

    folder_->setToolButtonStyle(Qt::ToolButtonTextOnly);
    folder_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    connect(folder_, &QToolButton::clicked, this, &NoteListPage::folderRequested);

    sort_->setMenu(sortMenu_);
    sort_->setPopupMode(QToolButton::InstantPopup);
    rebuildSortMenu();

    // Present, as the brief asks, and disabled: step 0 does not edit.
    newFolder_->setEnabled(false);
    newNote_->setEnabled(false);

    auto* bar = new QHBoxLayout;
    bar->setContentsMargins(4, 0, 4, 0);
    bar->addWidget(folder_, 1);
    bar->addWidget(sort_);
    bar->addWidget(newFolder_);
    bar->addWidget(newNote_);

    hint_->setWordWrap(true);
    hint_->setAlignment(Qt::AlignCenter);
    hint_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    hint_->setVisible(false);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addLayout(bar);
    layout->addWidget(hint_, 1);
    layout->addWidget(&panels_.listPanel(), 1);

    QListView& list = panels_.listView();
    // Kinetic scrolling by finger; the mouse path stays as it is.
    QScroller::grabGesture(list.viewport(), QScroller::TouchGesture);
    list.setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    // The desktop list drags notes onto folders; a drag on a phone is a scroll.
    list.setDragEnabled(false);
    list.viewport()->installEventFilter(this);
    connect(&list, &QAbstractItemView::clicked, this, [this](const QModelIndex& index) {
        const QString file = panels_.list().pathAt(index);
        if (!file.isEmpty()) emit noteTapped(file);
    });

    // The folder name follows the primary selection, whichever way it moved.
    connect(panels_.tree().selectionModel(), &QItemSelectionModel::currentChanged, this,
            [this](const QModelIndex&, const QModelIndex&) { syncFolderButton(); });
    connect(&panels_, &NotePanels::sortShown, this,
            [this](SortOrder order, bool fromMark) {
                sort_->setText(sortOrderTitle(order) + (fromMark ? QStringLiteral(" ·") : QString()));
            });
    syncFolderButton();
}

void NoteListPage::showNoStore(const QString& expectedRoot) {
    panels_.listPanel().setVisible(false);
    folder_->setEnabled(false);
    sort_->setEnabled(false);
    hint_->setText(QStringLiteral("No store on this phone yet.\n\n"
                                  "Expected at:\n%1\n\n"
                                  "Seed it from the mac:\n"
                                  "bash packaging/android/device.sh seed <store-dir>")
                       .arg(expectedRoot));
    hint_->setVisible(true);
}

void NoteListPage::refreshAppearance() { panels_.refreshAppearance(); }

void NoteListPage::syncFolderButton() {
    const QModelIndex current = panels_.tree().currentIndex();
    const QString title = current.isValid() ? panels_.model().titleOf(current) : QString();
    folder_->setText(title.isEmpty() ? QStringLiteral("All notes") : title);
}

void NoteListPage::rebuildSortMenu() {
    sortMenu_->clear();
    // The same three keys as the desktop toolbar. On the phone the choice
    // moves the ROOT switch only (state.json): step 0 writes nothing into
    // the store, so a folder's own mark keeps winning over it — exactly as
    // on the desktop when the root switch is used.
    for (const SortKey key : {SortKey::Modified, SortKey::Created, SortKey::Name}) {
        const SortOrder order = defaultOrder(key);
        QAction* action = sortMenu_->addAction(sortOrderTitle(order));
        connect(action, &QAction::triggered, this, [this, key] {
            bool fromMark = false;
            const SortOrder now = panels_.currentSort(&fromMark);
            panels_.setRootSort(pressedSort(now, key));
            panels_.syncSort();
        });
    }
}

bool NoteListPage::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() == QEvent::Paint && !firstPaintLogged_) {
        firstPaintLogged_ = true;
        perfLog("start → list painted", clock_.elapsed());
    }
    return QWidget::eventFilter(watched, event);
}

}  // namespace zametti
