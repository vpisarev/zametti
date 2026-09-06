// THE WIDTH MEMORY OF THE SIDE PANELS (books2, 06.09.2026).
//
// The panels are hidden and shown by one door — the toolbar button, the
// full-screen mode — and a splitter keeps the sizes of VISIBLE widgets only:
// panels hidden and shown again would come back collapsed. So the widths are
// remembered before hiding and put back on showing. Two things went wrong
// when this memory was a list next to the lambda in main():
//
//   - it was filled BEFORE THE WINDOW WAS LAID OUT. A session that opened in
//     the reading mode hid the panels while the store was being attached, i.e.
//     before window.show() and before the start widths were applied; the list
//     took the provisional sizes of an unshown splitter, and the person who
//     left the reading mode found both panels folded to a strip;
//   - the splitter state written to state.json at exit was taken while the
//     panels were hidden, so the next start began from folded widths too.
//
// Here the memory is filled only from a shown, laid-out window with the panels
// on screen, and the state for exit is the last one taken with the panels
// visible. Nothing remembered — the caller lays the widths out afresh, as on
// the first start.

#ifndef ZAMETTI_PANEL_MEMORY_H
#define ZAMETTI_PANEL_MEMORY_H

#include <QByteArray>
#include <QList>

class QSplitter;
class QWidget;

namespace zametti {

class NotePanels;

class PanelMemory {
public:
    PanelMemory(QSplitter& splitter, NotePanels& panels, QWidget& window);

    // Hide the panels; their widths are remembered if the window is shown
    // and the panels are on screen with real widths.
    void hide();
    // Show the panels and put the remembered widths back. False — nothing
    // was remembered, and the caller lays the widths out afresh.
    bool show();
    // The splitter state to write at exit: the live one while the panels are
    // visible, the one taken before hiding otherwise.
    QByteArray stateForExit() const;
    bool remembered() const { return !sizes_.isEmpty(); }

protected:
    bool laidOut() const;

    QSplitter& splitter_;
    NotePanels& panels_;
    QWidget& window_;
    QList<int> sizes_;
    QByteArray state_;
};

}  // namespace zametti

#endif  // ZAMETTI_PANEL_MEMORY_H
