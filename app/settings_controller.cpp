#include "settings_controller.h"

namespace zametti {

SettingsController::SettingsController(JsonEditView& view, std::shared_ptr<ZConfigFile> model,
                                       QObject* parent)
    : QObject(parent), view_(view), model_(std::move(model)) {
    // Esc из вида — выйти из режима (и записать). Вид про режим не знает.
    connect(&view_, &JsonEditView::leaveRequested, this, [this] { leave(); });
}

bool SettingsController::enter() {
    if (active_) return true;
    QString error;
    if (!model_->load(&error)) {
        emit saved(false, error);
        return false;
    }
    view_.setText(model_->text(), 0, 0);
    active_ = true;
    emit modeChanged(true);
    view_.setFocus();
    return true;
}

bool SettingsController::save() {
    if (!active_) return false;
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
