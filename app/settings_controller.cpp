#include "settings_controller.h"

#include <algorithm>

namespace zametti {

SettingsController::SettingsController(NoteEditor& editor, JsonEditView& view,
                                       std::shared_ptr<ZConfigFile> model, QObject* parent)
    : QObject(parent), editor_(editor), view_(view), model_(std::move(model)) {
    // Esc из вида — выйти из режима (и записать). Вид про режим не знает.
    connect(&view_, &JsonEditView::leaveRequested, this, [this] { leave(); });
    // ОТКРЫЛИ ЗАМЕТКУ — УХОДИМ (см. заголовок): человек ткнул в дерево или в
    // список, и показать ему надо заметку. Правка конфига уходит в файл тем же
    // путём, что по Ctrl+S. Сигнал «сменилась», а не «сейчас сменится»: у нас
    // ничего не живёт в самой заметке, торопиться некуда.
    connect(&editor_, &NoteEditor::fileChanged, this, [this](const QString&) { leave(); });
}

bool SettingsController::enter() {
    if (active_) return true;
    QString error;
    if (!model_->load(&error)) {
        emit saved(false, error);
        return false;
    }
    view_.setText(model_->text(), line_, column_);
    active_ = true;
    emit modeChanged(true);
    view_.setFocus();
    return true;
}

void SettingsController::setPlace(int line, int column) {
    line_ = std::max(0, line);
    column_ = std::max(0, column);
}

bool SettingsController::save() {
    if (!active_) return false;
    setPlace(view_.caretLine(), view_.caretColumn());
    model_->setText(view_.text());
    QString error;
    const bool ok = model_->save(&error);
    emit saved(ok, error);
    return ok;
}

bool SettingsController::leave() {
    if (!active_) return true;
    save();   // не записалось — человек узнает из полосы; в режиме не запираем
    active_ = false;
    emit modeChanged(false);
    return true;
}

bool SettingsController::toggle() {
    if (active_) {
        leave();
        return active_;
    }
    return enter();
}

}  // namespace zametti
