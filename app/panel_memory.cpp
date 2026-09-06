#include "panel_memory.h"

#include "note_panels.h"

#include <QSplitter>
#include <QWidget>

namespace zametti {

PanelMemory::PanelMemory(QSplitter& splitter, NotePanels& panels, QWidget& window)
    : splitter_(splitter), panels_(panels), window_(window) {}

bool PanelMemory::laidOut() const {
    // A window that is not shown has no widths of its own; a tree folded to
    // nothing is a splitter that was never given widths, not a wish.
    if (!window_.isVisible() || !panels_.wanted()) return false;
    const QList<int> sizes = splitter_.sizes();
    return sizes.size() == splitter_.count() && !sizes.isEmpty() && sizes.first() > 0;
}

void PanelMemory::hide() {
    if (laidOut()) {
        sizes_ = splitter_.sizes();
        state_ = splitter_.saveState();
    }
    panels_.setVisible(false);
}

bool PanelMemory::show() {
    panels_.setVisible(true);
    if (sizes_.size() != splitter_.count()) return false;
    splitter_.setSizes(sizes_);
    return true;
}

QByteArray PanelMemory::stateForExit() const {
    if (panels_.wanted() || state_.isEmpty()) return splitter_.saveState();
    return state_;
}

}  // namespace zametti
