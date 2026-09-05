#include "toc_list.h"

#include "note_view.h"
#include "settings.h"
#include "zapp.h"

#include <QEvent>
#include <QFontMetrics>
#include <QKeyEvent>
#include <QListWidget>
#include <QVBoxLayout>
#include <algorithm>

namespace zametti {

TocList::TocList(QWidget* parent) : QWidget(parent) {
    list_ = new QListWidget(this);
    list_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    list_->setFrameShape(QFrame::NoFrame);
    list_->installEventFilter(this);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(list_);
    setAutoFillBackground(true);
    refreshAppearance();
    connect(list_, &QListWidget::itemClicked, this, [this](QListWidgetItem* item) {
        choose(list_->row(item));
    });
    hide();
}

void TocList::follow(QWidget* anchor) {
    if (anchor_ != nullptr) anchor_->removeEventFilter(this);
    anchor_ = anchor;
    if (anchor_ != nullptr) anchor_->installEventFilter(this);
}

void TocList::refreshAppearance() {
    const ZSettings& a = settings();
    setFont(ZApp::instance().uiStyle().appFont());
    list_->setFont(font());
    QPalette pal = list_->palette();
    pal.setColor(QPalette::Base, a.ui().sidebarBackground());
    pal.setColor(QPalette::Window, a.ui().sidebarBackground());
    // The chosen line stays visible when the focus is in the text (as in the
    // history's list): both colour groups.
    for (QPalette::ColorGroup group : {QPalette::Active, QPalette::Inactive}) {
        pal.setColor(group, QPalette::Highlight, a.style().selectionBackground());
        pal.setColor(group, QPalette::HighlightedText, selectedTextColour(a.style(), pal));
    }
    list_->setPalette(pal);
    setPalette(pal);
}

void TocList::place() {
    if (anchor_ == nullptr) return;
    const QRect box = anchor_->geometry();
    // A third of the text, no wider than forty letters of the shell font.
    const int letters = QFontMetrics(font()).horizontalAdvance(QLatin1Char('A')) * 40;
    const int width = std::min(box.width() / 3, letters);
    setGeometry(box.right() - width + 1, box.top(), width, box.height());
    raise();
}

void TocList::open(std::shared_ptr<ZNote> note, int currentBlock) {
    note_ = std::move(note);
    list_->clear();
    int current = -1;
    if (note_ != nullptr) {
        const auto& outline = note_->outline();
        for (size_t i = 0; i < outline.size(); ++i) {
            const ZDocument::OutlineEntry& entry = outline[i];
            auto* item = new QListWidgetItem(
                QString(2 * std::max(0, entry.level - 1), QLatin1Char(' ')) + entry.text, list_);
            item->setData(Qt::UserRole, entry.block);
            if (entry.block <= currentBlock) current = int(i);
        }
    }
    if (list_->count() == 0) {
        auto* item = new QListWidgetItem(QStringLiteral("no headings"), list_);
        item->setFlags(Qt::NoItemFlags);
    }
    place();
    show();
    if (current >= 0) list_->setCurrentRow(current);
    else if (list_->count() > 0 && list_->item(0)->flags() != Qt::NoItemFlags) list_->setCurrentRow(0);
    list_->setFocus(Qt::OtherFocusReason);
}

void TocList::close() {
    if (!isVisible() || closing_) return;
    closing_ = true;
    hide();
    note_.reset();
    emit closed();
    closing_ = false;
}

void TocList::choose(int row) {
    if (row < 0 || row >= list_->count()) return;
    const QListWidgetItem* item = list_->item(row);
    if (item->flags() == Qt::NoItemFlags) return;
    const int block = item->data(Qt::UserRole).toInt();
    close();
    emit headingChosen(block);
}

bool TocList::eventFilter(QObject* watched, QEvent* event) {
    if (watched == anchor_ && (event->type() == QEvent::Resize || event->type() == QEvent::Move)) {
        if (isVisible()) place();
        return false;
    }
    if (watched == list_) {
        // A click anywhere else takes the focus away: the list goes, as on Esc.
        if (event->type() == QEvent::FocusOut && isVisible()) close();
        if (event->type() == QEvent::KeyPress) {
            auto* key = static_cast<QKeyEvent*>(event);
            if (key->key() == Qt::Key_Escape) {
                close();
                return true;
            }
            if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) {
                choose(list_->currentRow());
                return true;
            }
        }
        if (event->type() == QEvent::ShortcutOverride) {
            auto* key = static_cast<QKeyEvent*>(event);
            if (key->key() == Qt::Key_Escape || key->key() == Qt::Key_Return ||
                key->key() == Qt::Key_Enter || key->key() == Qt::Key_Up || key->key() == Qt::Key_Down) {
                event->accept();
                return true;
            }
        }
    }
    return QWidget::eventFilter(watched, event);
}

void TocList::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape) {
        close();
        return;
    }
    QWidget::keyPressEvent(event);
}

bool TocList::event(QEvent* event) {
    if (event->type() == QEvent::ShortcutOverride) {
        auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Escape) {
            event->accept();
            return true;
        }
    }
    return QWidget::event(event);
}

}  // namespace zametti
