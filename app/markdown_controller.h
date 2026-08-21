// КОНТРОЛЛЕР РЕЖИМА ПРАВКИ ИСХОДНИКА: вход, выход, смена заметки, клавиши.
//
// MVC режима: модель — сама заметка (ZDocument умеет отдать свой исходник,
// перевести каретку и принять правленое одним глаголом applySourceText), вид —
// MarkdownEditView (плоский текст с подсветкой), контроллер — здесь.
//
// Отдельным объектом, а не лямбдами в main(): проводка режима — ровно то, что
// ломается молча, и набор обязан звать ту же проводку, что и окно. Тот же довод
// и то же устройство, что у HistoryController.
//
// РЕЖИМ ПРИНАДЛЕЖИТ ПРИЛОЖЕНИЮ, А НЕ ЗАМЕТКЕ (решение владельца): по заметкам
// ходят, а кнопка [M] остаётся нажатой, и каждая следующая заметка открывается
// исходником. Признак живёт в state.json и переживает перезапуск. Отсюда
// noteChanged(): заметку сменили — вид перезаливается, режим не гаснет.

#ifndef ZAMETTI_MARKDOWN_CONTROLLER_H
#define ZAMETTI_MARKDOWN_CONTROLLER_H

#include "editor_widget.h"
#include "markdown_edit_view.h"

#include <QObject>

namespace zametti {

class HistoryController;

class MarkdownController : public QObject {
    Q_OBJECT

public:
    MarkdownController(NoteEditor& editor, MarkdownEditView& view, QObject* parent = nullptr);

    // ВХОД — ЕЩЁ ОДНА ТОЧКА СОХРАНЕНИЯ, в один ряд с входом в историю и уходом
    // из заметки: незаписанные правки уходят в файл до того, как истина
    // переедет в текст. false — открытой заметки нет, показывать нечего.
    bool enter();
    // ВЫХОД: наложить правленое, вернуть каретку, снять флаг, сохранить.
    // Возвращает то же, что applySourceText (см. document.h): ≥ 0 — сколько
    // кусков наложено, < 0 — текст не принят, и режим НЕ закрывается.
    int leave();
    bool toggle();
    bool active() const { return active_; }

    // Заметку сменили, а режим идёт: наложить правленое в СТАРУЮ (это делает
    // окно до смены) и перезалить вид новой. Зовётся ПОСЛЕ открытия новой.
    void refill();
    // Сохранить, не выходя из режима: наложить и записать. Ctrl+S в режиме.
    int saveWithoutLeaving();

    void refreshAppearance();

    // РЕЖИМ ПЕРЕЖИВАЕТ ПОХОД В ИСТОРИЮ (просьба владельца, refactor3): markdown
    // знает историю (одна сторона; история про markdown не знает). Вход в
    // историю при идущем режиме — наложить и отложить (suspended_); выход из
    // истории — вернуть режим с ТЕКУЩИМ исходником заметки (после
    // восстановления из слепка — восстановленным: restore — обычная правка
    // заметки, одним шагом отмены, и переключение обратно без правок стек не
    // трогает). Ctrl+Z на дне стека режима, ушедший в историю, — тот же путь;
    // отменивший шаг самой заметки — остаётся в обычном виде (сценарий A
    // владельца: там лежит атомарная правка, собранная из прошлого захода).
    // Окно и набор зовут одно и то же.
    void attachHistory(HistoryController& history);

signals:
    void modeChanged(bool on);
    // Текст не принят: −1 — в исходнике набрана шапка `<!-- zametti`,
    // −2 — наложение разошлось с каноном и отменено (дефект, не поведение).
    void applyRefused(int code);

private:
    int apply();
    void resume();

    NoteEditor& editor_;
    MarkdownEditView& view_;
    HistoryController* history_ = nullptr;
    bool active_ = false;
    bool suspended_ = false;
};

}  // namespace zametti

#endif  // ZAMETTI_MARKDOWN_CONTROLLER_H
