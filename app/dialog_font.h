// Шрифт диалогов — ТОТ ЖЕ, ЧТО У ВСЕЙ ОБОЛОЧКИ.
//
// Диалоги на голых Qt-виджетах рисовались системным шрифтом — и на FullHD-
// ноуте владельца выходили мельче остальной программы («такое ощущение, что
// где-то scale-factor учитываем, а где-то нет», п.6 первого живого прогона).
// Плотность экрана ни при чём: это «жёсткий кегль против системного дефолта».
//
// Теперь у оболочки один расчёт на всё (ui_style.h), и он же ставится
// приложению целиком (QApplication::setFont в applyAppearance) — так шрифт
// достаётся и меню, и подсказкам, и тем виджетам диалогов, до которых
// setFont на корне не дотягивается. Функция остаётся: она короче и говорит
// вслух, ЧТО именно берётся.

#ifndef ZAMETTI_DIALOG_FONT_H
#define ZAMETTI_DIALOG_FONT_H

#include "zapp.h"

#include <QFont>

namespace zametti {

inline QFont dialogFont() { return ZApp::instance().uiStyle().appFont(); }

}  // namespace zametti

#endif  // ZAMETTI_DIALOG_FONT_H
