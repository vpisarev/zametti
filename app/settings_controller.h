// КОНТРОЛЛЕР РЕЖИМА ПРАВКИ НАСТРОЕК: вход, выход, запись, применение.
//
// MVC режима (refactor3, просьба владельца «вместо открытия config.json во
// внешнем редакторе открываем внутренний»): модель — ZConfigFile (ядро: файл,
// текст, атомарная запись, проверка), вид — JsonEditView (плоский текст с
// подсветкой JSON), контроллер — здесь. Устройство и доводы те же, что у
// MarkdownController: проводка режима — ровно то, что ломается молча, и набор
// обязан звать ту же проводку, что и окно.
//
// ЗАПИСЬ — ПО Ctrl+S И ПРИ УХОДЕ ИЗ РЕЖИМА (решение владельца; диалогов
// подтверждения у нас нет). Битый JSON тоже записывается — это файл человека,
// — но программа его не применяет и говорит об этом в полосе сведений: тем же
// путём, что и при внешней правке файла. Применяет окно, одним местом.

#ifndef ZAMETTI_SETTINGS_CONTROLLER_H
#define ZAMETTI_SETTINGS_CONTROLLER_H

#include "config_file.h"
#include "editor_widget.h"
#include "json_edit_view.h"

#include <QObject>
#include <QString>

#include <memory>

namespace zametti {

class SettingsController : public QObject {
    Q_OBJECT

public:
    // Редактор нужен затем, чтобы уйти с дороги: ОТКРЫЛИ ЗАМЕТКУ — РЕЖИМ
    // ЗАКРЫВАЕТСЯ (решение владельца). Щёлкнув по заметке в дереве, человек
    // просит показать её, а не конфиг; правка при этом не теряется — выход
    // пишет файл, как Ctrl+S. Режим исходника ведёт себя иначе нарочно: он
    // принадлежит заметкам и по ним ходят, не выходя из него.
    SettingsController(NoteEditor& editor, JsonEditView& view,
                       std::shared_ptr<ZConfigFile> model, QObject* parent = nullptr);

    // Открыть конфиг (нет файла — записать шаблон и открыть его). false — файл
    // не читается и не пишется; причина уходит сигналом saved(false, …).
    bool enter();
    // Записать и выйти. Всегда true: отказаться от выхода режим не вправе.
    bool leave();
    bool toggle();
    bool active() const { return active_; }
    // Ctrl+S: записать, не выходя из режима.
    bool save();

    void refreshAppearance() { view_.refreshAppearance(); }

    // WHERE THE CARET WAS (the owner, 07.09.2026): trying values means going
    // in and out of the config many times, and scrolling to the same key
    // every time is the cost that made it tiresome. The place is remembered
    // on every save and leave, restored on enter, and the window keeps it
    // across restarts in state.json (ZAppState::configLine/Column).
    int line() const { return line_; }
    int column() const { return column_; }
    void setPlace(int line, int column);

signals:
    void modeChanged(bool on);
    // Записан ли файл (и почему нет). Окно на этом перечитывает настройки.
    void saved(bool ok, const QString& error);

private:
    NoteEditor& editor_;
    JsonEditView& view_;
    std::shared_ptr<ZConfigFile> model_;
    bool active_ = false;
    int line_ = 0;
    int column_ = 0;
};

}  // namespace zametti

#endif  // ZAMETTI_SETTINGS_CONTROLLER_H
