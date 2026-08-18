// Режим истории вокруг уже существующего редактора — для наборов, которые
// проверяют редактор, а в историю заходят по дороге (editor_test,
// history_write_test, history_search_test).
//
// Проводка та же, что в окне: HistoryView (баннер, разность, список записей) +
// HistoryController над данным NoteEditor. Вид живёт без родителя и умирает
// вместе с рамкой.

#ifndef ZAMETTI_TESTS_HISTORY_RIG_H
#define ZAMETTI_TESTS_HISTORY_RIG_H

#include "editor_widget.h"
#include "history_controller.h"
#include "history_panel.h"
#include "history_view.h"

#include <QString>
#include <QTextBlock>

namespace zt {

struct HistoryRig {
    zametti::HistoryView view;
    zametti::HistoryController controller;

    explicit HistoryRig(zametti::NoteEditor& editor) : controller(editor, view) {}

    bool enter(int index = -1) { return controller.enter(index); }
    void leave() { controller.leave(); }
    bool active() const { return controller.active(); }
    int index() const { return controller.index(); }
    // Текст показанного документа разности построчно.
    QString shownText() {
        QString out;
        for (QTextBlock b = view.textView().document()->begin(); b.isValid(); b = b.next()) {
            out += b.text();
            out += QLatin1Char('\n');
        }
        return out;
    }
};

}  // namespace zt

#endif  // ZAMETTI_TESTS_HISTORY_RIG_H
