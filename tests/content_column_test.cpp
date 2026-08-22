// КОЛОНКА ТЕКСТА — ОДИН РАСЧЁТ НА ВСЕ ВИДЫ (решение владельца: «код
// переиспользовать, а не писать одно и то же два раза и повторять те же баги
// два раза»).
//
// Правило колонки жило в двух местах: у вида заметки (QTextEdit) и у плоских
// видов исходника и настроек (QPlainTextEdit). Оба считали одно и то же — и
// оба поодиночке ловили одну и ту же беду с шириной. Теперь расчёт один
// (content_column.h), и спрашивается он здесь.

#include "content_column.h"
#include "settings.h"
#include "settings_hook.h"

#include "test_util.h"

#include <string>

namespace {

zametti::ZDocStyle styleWith(qreal side, qreal maxWidth) {
    zametti::ZSettings settings;
    settings.style().setSideMargin(side);
    settings.style().setMaxContentWidth(maxWidth);
    return settings.style();
}

}  // namespace

TEST(ContentColumn, All) {
    // Узкое окно: делить нечего — поле ровно боковое (4 ширины «A» по 10 px).
    {
        const zametti::ZDocStyle style = styleWith(4.0, 20.0);
        ZT_EQ("в узком окне поле — боковое", std::string("40"),
              std::to_string(zametti::contentColumnMargin(style, 10.0, /*room=*/250)));
    }
    // Широкое окно: лишнее уходит в поля, колонка встаёт ровно по потолку.
    {
        const zametti::ZDocStyle style = styleWith(4.0, 80.0);
        const int room = 2000;
        const int margin = zametti::contentColumnMargin(style, 10.0, room);
        const int column = room - 2 * margin;
        ZT_EQ("колонка встала по потолку (80 × 10)", std::string("800"), std::to_string(column));
    }
    // Боковое поле документа зачитывается: вид заметки уже получил его от
    // сборщика, и отнимать столько же второй раз нельзя.
    {
        const zametti::ZDocStyle style = styleWith(4.0, 20.0);
        ZT_EQ("документ дал всё поле — виду добавлять нечего", std::string("0"),
              std::to_string(zametti::contentColumnMargin(style, 10.0, 250, /*fromDocument=*/40.0)));
        ZT_EQ("документ дал половину — вид добавляет вторую", std::string("20"),
              std::to_string(zametti::contentColumnMargin(style, 10.0, 250, /*fromDocument=*/20.0)));
        ZT_EQ("документ дал больше нужного — вид не отнимает назад", std::string("0"),
              std::to_string(zametti::contentColumnMargin(style, 10.0, 250, /*fromDocument=*/99.0)));
    }
    // Масштаб: буква шире — колонка шире, полям остаётся меньше.
    {
        const zametti::ZDocStyle style = styleWith(4.0, 80.0);
        const int small = zametti::contentColumnMargin(style, 10.0, 2000);
        const int large = zametti::contentColumnMargin(style, 20.0, 2000);
        ZT_TRUE("на большем кегле поле меньше (" + std::to_string(small) + " > " +
                    std::to_string(large) + ")",
                large < small);
    }
    zt::report("колонка текста: общий расчёт");
}
