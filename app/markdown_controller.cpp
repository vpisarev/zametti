#include "markdown_controller.h"

#include "history_controller.h"

#include <QTextCursor>

namespace zametti {

MarkdownController::MarkdownController(NoteEditor& editor, MarkdownEditView& view,
                                       QObject* parent)
    : QObject(parent), editor_(editor), view_(view) {
    // ESC ИЗ РЕЖИМА ИСХОДНИКА НЕ ВЫВОДИТ (решение владельца, refactor3): выйти
    // можно кнопкой [M]. Довод — цена ошибки: Esc под рукой, а выход из
    // исходника накладывает всю набранную работу на заметку; у правки настроек
    // он остался, там выход безвреден. Сигнал вида не слушаем вовсе — так это
    // видно в одном месте, а не прячется в условии.
    // Ctrl+Z на дне стека режима: правок в тексте больше нет (всё отменено —
    // или их и не было), режим закрывается БЕЗ потерь (наложить нечего), и
    // отмена уходит заметке — её стек, а за ним слепки журнала, как всегда.
    // Так у человека один ряд Ctrl+Z: правки исходника → правки заметки →
    // история.
    connect(&view_, &MarkdownEditView::undoExhausted, this, [this] {
        if (leave() < 0) return;   // текст не принят — из режима не выпускаем
        editor_.undo();
        // Отмена ушла в историю (стек заметки был пуст) — режим отложен и
        // вернётся по выходу из неё. Отменила шаг заметки — остаёмся в обычном
        // виде (сценарий A владельца).
        if (history_ != nullptr && history_->active()) suspended_ = true;
    });
    // ЗАМЕТКА МЕНЯЕТСЯ, А РЕЖИМ ИДЁТ. Правки живут в тексте вида, и наложить их
    // можно только пока прежняя заметка ещё открыта: сигнал приходит ДО подмены.
    connect(&editor_, &NoteEditor::fileAboutToChange, this, [this](const QString&) {
        if (active_) apply();
    });
    // А после подмены — залить вид новой заметкой. Режим не гаснет: он
    // принадлежит приложению, а не заметке.
    connect(&editor_, &NoteEditor::fileChanged, this, [this](const QString&) { refill(); });
}

void MarkdownController::attachHistory(HistoryController& history) {
    history_ = &history;
    connect(&history, &HistoryController::modeChanged, this, [this](bool on) {
        if (on) {
            // Идущий режим откладывается: наложить и записать — вершина работы
            // обязана оказаться в истории, куда человек идёт смотреть.
            if (!active_) return;
            if (leave() < 0) return;   // текст не принят — режим не отложен
            suspended_ = true;
            return;
        }
        resume();
    });
}

void MarkdownController::resume() {
    if (!suspended_) return;
    suspended_ = false;
    enter();   // текущий исходник заметки — после restore он восстановленный
}

bool MarkdownController::enter() {
    suspended_ = false;   // явный вход — не возобновление
    if (active_) return true;
    if (editor_.filePath().isEmpty()) return false;
    // В режим — из истории, а не поверх неё: две страницы стека разом быть
    // активными не должны.
    if (history_ != nullptr && history_->active()) history_->leave();

    // ТОЧКА СОХРАНЕНИЯ. Человек уходит править исходник — вершина его работы
    // обязана оказаться в файле и в журнале до того, как истина переедет в
    // текст. Признак «изменён» не спрашиваем: лишней записи не бывает, правила
    // отбора внутри save работают.
    editor_.save(false, /*force=*/true);

    ZDocument& note = editor_.note();
    const SourcePos caret = note.sourcePosOf(editor_.textCursor());
    // ТЕЛО БЕЗ ШАПКИ. Шапкой владеет ZNote, и `modified` в ней меняется при
    // каждой записи: в исходнике ей делать нечего — она мигала бы под руками.
    view_.showSource(note.toMarkdownText(), caret);
    note.setSourceEditing(true);

    active_ = true;
    emit modeChanged(true);
    view_.setFocus();
    return true;
}

int MarkdownController::apply() {
    ZDocument& note = editor_.note();
    // ФЛАГ СНИМАЕТСЯ ДО НАЛОЖЕНИЯ: пока он стоит, заметка правку не принимает
    // вовсе — тем он и полезен. Не приняли текст — ставим обратно.
    note.setSourceEditing(false);
    const SourcePos caret = view_.caretPos();
    const int hunks = note.applySourceText(view_.source());
    if (hunks < 0) {
        note.setSourceEditing(true);
        emit applyRefused(hunks);
        return hunks;
    }
    editor_.setTextCursor(note.cursorAtSourcePos(caret));
    return hunks;
}

int MarkdownController::leave() {
    if (!active_) return 0;
    const int hunks = apply();
    if (hunks < 0) return hunks;   // текст не принят — из режима не выпускаем
    active_ = false;
    emit modeChanged(false);
    editor_.save(false);
    editor_.setFocus();
    return hunks;
}

bool MarkdownController::toggle() {
    if (active_) {
        leave();
        return active_;
    }
    return enter();
}

int MarkdownController::saveWithoutLeaving() {
    if (!active_) return 0;
    const int hunks = apply();
    if (hunks < 0) return hunks;
    // Наложили — и снова отдаём заметку тексту: режим продолжается.
    editor_.note().setSourceEditing(true);
    editor_.save(false);
    return hunks;
}

void MarkdownController::refill() {
    if (!active_) return;
    ZDocument& note = editor_.note();
    const SourcePos caret = note.sourcePosOf(editor_.textCursor());
    view_.showSource(note.toMarkdownText(), caret);
    note.setSourceEditing(true);
    view_.setFocus();
}

void MarkdownController::refreshAppearance() { view_.refreshAppearance(); }

}  // namespace zametti
