// ЛЮК НАБОРОВ: правка настроек. В боевых заголовках его нет намеренно —
// ZSettings только для чтения (решение владельца), а нужный набору облик или
// бюджет он подсовывает сюда, пока класс под проверкой не научится брать свою
// часть настроек параметром (тогда набор передаст часть напрямую, и люк
// станет не нужен). Всякий набор, тронувший настройки, обязан вернуть их
// как были: наборы идут одним процессом.
#ifndef ZAMETTI_TESTS_SETTINGS_HOOK_H
#define ZAMETTI_TESTS_SETTINGS_HOOK_H

#include "settings.h"
#include "settings_hook.h"

namespace zametti {
ZSettings& mutableSettingsForTests();
}

#endif
