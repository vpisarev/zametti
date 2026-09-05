// Место человека в заметке — все три числа сразу: «где каретка» и «что
// выделено» — разные вопросы, и заметка обязана помнить оба (просьба
// владельца), а прокрутка — где он был на экране. Хранит ZNote; переживает
// выход заметки из кэша и перезапуск через состояние приложения (ZAppState).

#ifndef ZAMETTI_CARET_SPOT_H
#define ZAMETTI_CARET_SPOT_H

namespace zametti {

struct CaretSpot {
    int cursor = 0;
    int anchor = 0;
    int scroll = 0;
    // THE READING PLACE (brief 18): the line at the top of the left page,
    // as (block, line) — pixels would not survive a zoom or a resize. And
    // the mode the note was left in: 0 — never chosen (a book opens in the
    // reading mode, a note in the editor), 1 — reading, 2 — editing.
    int readingBlock = 0;
    int readingLine = 0;
    int mode = 0;
};

}  // namespace zametti

#endif  // ZAMETTI_CARET_SPOT_H
