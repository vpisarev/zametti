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
};

}  // namespace zametti

#endif  // ZAMETTI_CARET_SPOT_H
