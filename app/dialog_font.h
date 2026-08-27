// Шрифт диалогов — ИЗ НАСТРОЕК, а не системный дефолт.
//
// Общего QApplication::setFont в программе нет: свои виджеты (дерево, список,
// полоса сведений) берут кегль из настроек, а диалоги на голых Qt-виджетах
// рисовались системным шрифтом — и на FullHD-ноуте владельца выходили мельче
// остальной программы («такое ощущение, что где-то scale-factor учитываем, а
// где-то нет», п.6 первого живого прогона). Плотность экрана ни при чём:
// это «жёсткий кегль против системного дефолта». Лечение — одно место, откуда
// диалог берёт шрифт боковых панелей, и setFont на корне (дети наследуют).

#ifndef ZAMETTI_DIALOG_FONT_H
#define ZAMETTI_DIALOG_FONT_H

#include "settings.h"

#include <QFont>

namespace zametti {

inline QFont dialogFont() {
    const ZSettings& a = settings();
    QFont font(a.ui().sidebarFontFamily().isEmpty() ? a.style().fontFamily()
                                                    : a.ui().sidebarFontFamily());
    font.setPointSizeF(a.ui().sidebarFontPoint());
    return font;
}

}  // namespace zametti

#endif  // ZAMETTI_DIALOG_FONT_H
