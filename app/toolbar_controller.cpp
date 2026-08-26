#include "toolbar_controller.h"

#include "editor_widget.h"
#include "reader_view.h"
#include "toolbar.h"

namespace zametti {

ToolbarController::ToolbarController(Toolbar& bar, NoteEditor& editor, ReaderView& docs,
                                     Reader read, QObject* parent)
    : QObject(parent), bar_(bar), read_(std::move(read)) {
    // СМЕНА ЗАМЕТКИ — первый повод, и главный: свойства заметки (заперта,
    // архивная) решают половину правила. Дверей к смене много — дерево,
    // список, поиск, восстановление из истории, старт, — а сигнал один.
    connect(&editor, &NoteEditor::fileChanged, this, [this](const QString&) { refresh(); });
    // СМЕНА ПОКАЗАННОГО ДОКУМЕНТА — второй. Он приходит и когда документацию
    // открыли, и когда её убрали, — то есть ровно тот случай, на котором
    // обожглись: возврат к заметке гасил страницу, но не зажигал кнопки.
    connect(&docs, &ReaderView::shownChanged, this, [this](const QString&) { refresh(); });
    refresh();
}

void ToolbarController::refresh() {
    const ToolbarState state = read_();
    // По списку кнопок, а не по написанному руками перечню: завели новую
    // кнопку — она сразу подчиняется правилу, и забыть её негде.
    for (const Toolbar::Spec& spec : Toolbar::specs())
        bar_.setPromise(spec.id, toolbarPromiseFor(spec.id, state));
}

}  // namespace zametti
