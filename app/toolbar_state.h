// КАКИЕ КНОПКИ ТУЛБАРА ДОСТУПНЫ — ПРАВИЛО ОТДЕЛЬНО ОТ ПРОВОДКИ.
//
// Правило это уже однажды подвело человека, и подвело молча: доступность
// считало одно место в окне, а звали его только при смене хранилища и при
// показе документации. Открыл документ папки Info, вернулся к заметке — и
// кнопки так и остались серыми, с подсказкой «documentation is read-only», хотя
// документации на экране давно нет (нашёл владелец 27.08.2026). Позвать пересчёт
// из места, где заметка сменяется, было физически нельзя: та лямбда объявлена
// раньше.
//
// Поэтому здесь — ЧИСТАЯ ФУНКЦИЯ и плоский набор признаков, без ссылок на живые
// объекты: так правило проверяется перебором, без единого виджета, а окно
// остаётся тем, кто отвечает лишь на вопрос «что у меня сейчас на виду».
// Тем же приёмом живут `actionFor` (слой объектов) и `zoomTargetFor` (масштаб).
//
// ЗДЕСЬ ТОЛЬКО «ПОЧЕМУ НЕЛЬЗЯ». Нажатость (`setChecked` у режимов и сортировок),
// цвет и значок — не про доступность, и правило их не касается.

#ifndef ZAMETTI_TOOLBAR_STATE_H
#define ZAMETTI_TOOLBAR_STATE_H

#include "toolbar.h"

#include <QString>

namespace zametti {

// Что окно знает о себе в этот миг.
struct ToolbarState {
    // Хранилище открыто. Нет — горит одна кнопка, «открыть хранилище»: пустое
    // окно обязано подсказывать само (решение владельца).
    bool store = false;
    // На виду вшитый документ (папка Info). За ним нет ни файла в хранилище,
    // ни журнала — истории у него не бывает вовсе.
    bool documentation = false;
    // Открытая заметка заперта меткой `access: read-only` — своей или папки над
    // ней. Впереди ввезённые книжки, у которых это обычное состояние.
    bool readOnlyNote = false;
    // Открытая заметка убрана в архив: правится она только после возврата.
    bool archivedNote = false;
    // THE SOFT LOCK (`lock: yes`, brief 18): typing is refused, the lock
    // button shows a closed lock. tempUnlocked — the person opened it "until
    // I switch notes": the button shows an open lock in the accent colour.
    bool lockedNote = false;
    bool tempUnlocked = false;
    // A book (`role: book`): its history is not shown — the journal keeps
    // going (it carries the book to the cloud), but a history of one's own
    // marks and comments is not a history worth a mode (owner's decision,
    // 05.09.2026).
    bool book = false;
    // The reading mode is on (brief 18): the side panels are hidden for its
    // duration and their button is dark — the book takes the whole window.
    bool reading = false;
    // A flat view is on screen — the source, the settings, a history diff:
    // there is no outline to show there.
    bool flatView = false;
    // Синхронизация настроена. Не настроена — кнопка облака не поломка, а
    // состояние, и подсказка говорит словами, что сделать.
    bool cloudConfigured = false;
    QString cloudStatus;
};

// Почему кнопка недоступна; пустая строка — доступна.
//
// Слова причины видит человек в подсказке, и они же — то, по чему набор
// отличает «погашено за дело» от «погашено неизвестно почему».
inline QString toolbarPromiseFor(Toolbar::Button id, const ToolbarState& state) {
    using Button = Toolbar::Button;

    // БЕЗ ХРАНИЛИЩА ГОРИТ ОДНА КНОПКА. Она же — единственный намёк, который
    // человек в пустом окне получит: делать здесь можно ровно одно.
    if (id == Button::OpenStore) return {};
    if (!state.store) return QStringLiteral("no storage is open");

    // ЧИТАЮТ — НЕ ЗНАЧИТ ПРАВЯТ. На документации и на запертой заметке гаснет
    // то, что ПИШЕТ: правка исходника и ввоз картинок в открытую заметку.
    // Вывоз и импорт .md здесь ни при чём — читать и уносить прочитанное никто
    // не запрещал.
    const bool locked = state.documentation || state.readOnlyNote || state.archivedNote;
    if (locked && (id == Button::MarkdownEdit || id == Button::InsertImages))
        return state.documentation ? QStringLiteral("documentation is read-only")
                                   : QStringLiteral("this note is read-only");
    // История у архивной и запертой заметки работает как у всякой другой (так
    // решено раньше): смотреть прошлое не значит его менять. Гаснет она только
    // на документации, у которой прошлого нет вовсе.
    if (state.documentation && id == Button::History)
        return QStringLiteral("documentation is read-only");
    if (state.book && id == Button::History)
        return QStringLiteral("a book keeps its journal, but shows no history");
    if (id == Button::Panels && state.reading)
        return QStringLiteral("the side panels come back when the reading ends");
    if (id == Button::Toc && (state.flatView || state.documentation))
        return state.documentation ? QStringLiteral("documentation has no table of contents")
                                   : QStringLiteral("no table of contents in this view");
    // The reading mode shows the OPEN note's document; the documentation and
    // an archived note are shown by pages of their own.
    if (id == Button::Reading && (state.documentation || state.archivedNote))
        return state.documentation ? QStringLiteral("documentation has a page of its own")
                                   : QStringLiteral("an archived note is shown as it was");
    // The lock is a mark of the note's own header: nothing to write on the
    // documentation, and a frozen or archived note is not unlocked by it.
    if (id == Button::Lock && locked)
        return state.documentation ? QStringLiteral("documentation is read-only")
             : state.archivedNote  ? QStringLiteral("an archived note is read-only")
                                   : QStringLiteral("this note is read-only");

    // Облако: кнопка живая только у настроенного синка.
    if (id == Button::Cloud && !state.cloudConfigured) return state.cloudStatus;

    return {};
}

}  // namespace zametti

#endif  // ZAMETTI_TOOLBAR_STATE_H
