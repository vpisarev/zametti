// КОЛОНКА ТЕКСТА — ОДИН РАСЧЁТ НА ВСЕ ВИДЫ (решение владельца: «код
// переиспользовать, а не писать одно и то же два раза и повторять те же баги
// два раза»).
//
// Ширину колонки задаёт облик: боковое поле (sideMargin, в ширинах буквы «A»)
// и потолок ширины (maxContentWidth). Лишнее место широкого окна уходит в поля
// ВЬЮПОРТА, а не документа: всякая запись формата попадает в штатный стек
// отмены, и перекладка окна шагом Ctrl+Z быть не имеет права.
//
// Считают это двое: вид заметки (QTextEdit) и плоские виды исходника и
// настроек (QPlainTextEdit). Пород две, правило одно — и живёт оно здесь.

#ifndef ZAMETTI_CONTENT_COLUMN_H
#define ZAMETTI_CONTENT_COLUMN_H

#include "settings.h"

#include <algorithm>

namespace zametti {

// Сколько отнять у вьюпорта с каждой стороны.
//
//   charUnit     — ширина буквы «A» НЫНЕШНИМ шрифтом: поле обязано расти вместе
//                  с масштабом, иначе на 200 % текст прижимается к краю окна;
//   room         — вся ширина, из которой раздаётся место: нынешний вьюпорт
//                  плюс то, что мы у него уже отняли (по width() виджета
//                  считать нельзя — там ещё рамка и полоса прокрутки, и вышла
//                  бы обратная связь);
//   fromDocument — боковое поле, которое документ уже даёт сам (его ставит
//                  сборщик заметки); у плоских видов такого нет — там ноль.
inline int contentColumnMargin(const ZDocStyle& style, qreal charUnit, int room,
                               qreal fromDocument = 0.0) {
    const qreal want = style.sideMargin() * charUnit;
    qreal margin = std::max(0.0, want - fromDocument);
    if (style.maxContentWidth() > 0.0) {
        const qreal limit = style.maxContentWidth() * charUnit;
        const qreal spare = (room - 2 * want - limit) / 2;
        if (spare > 0.0) margin += spare;
    }
    return std::max(0, int(margin));
}

}  // namespace zametti

#endif
