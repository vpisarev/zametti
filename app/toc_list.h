// TocList — the table of contents of the open note (brief 18).
//
// A sticky button on the toolbar shows the headings of the note in a list at
// the right edge of the text; the person picks one (click or Enter) and the
// text goes there, or presses Esc and stays — either way the list hides at
// once and the button pops back up (owner's decision, 05.09.2026). Nothing of
// it persists: no splitter, no width in the state. The headings come from
// ZNote::outline(), fresh for the document as it is now.
#ifndef ZAMETTI_TOC_LIST_H
#define ZAMETTI_TOC_LIST_H

#include "znote.h"

#include <QWidget>
#include <memory>

class QListWidget;

namespace zametti {

class TocList : public QWidget {
    Q_OBJECT

public:
    explicit TocList(QWidget* parent = nullptr);

    // The widget whose right edge the list overlays (the text stack): the
    // list follows its geometry while open.
    void follow(QWidget* anchor);
    // Fill from the note and show, the heading above the given block selected
    // and the list focused. No headings — the list shows one grey line saying so.
    void open(std::shared_ptr<ZNote> note, int currentBlock);
    // Hide; closed() tells the window.
    void close();
    bool isOpen() const { return isVisible(); }
    void refreshAppearance();
    // THE LIST IS SET OFF FROM THE TEXT (owner's wish, 06.09.2026): a thin
    // dark-grey line down its left edge and a small inset before the entries.
    // The inset in pixels, line included.
    int leftInset() const { return inset_; }
    int edgeWidth() const { return edge_; }

signals:
    // A heading was picked: its block number.
    void headingChosen(int block);
    void closed();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    bool event(QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

private:
    void place();
    void choose(int row);

    QListWidget* list_ = nullptr;
    QWidget* anchor_ = nullptr;
    int inset_ = 0;
    int edge_ = 1;
    std::shared_ptr<ZNote> note_;
    bool closing_ = false;
};

}  // namespace zametti

#endif  // ZAMETTI_TOC_LIST_H
