// КАРЕТКА ОДНОСТРОЧНЫХ ПОЛЕЙ — та же, что в заметке: цвет и толщина из
// настроек, мигание общим CaretBlink.
//
// Поля правки подписи картинки и языка блока кода — QLineEdit, и своей каретки
// он не даёт погасить (setCursorWidth есть только у QPlainTextEdit/QTextEdit).
// Поэтому приём тот же, что у видов (qt-caret-facts): рисуем ПОВЕРХ штатной —
// в фазе «горит» кладём свою полосу, в фазе «погасла» закрашиваем место
// каретки фоном поля, иначе сквозь неё мигала бы чужая тонкая чёрточка.
//
// Фон однотонный (поле само его заливает, см. lang_editor.cpp), поэтому
// затирание честное: под кареткой ничего, кроме заливки, нет.

#ifndef ZAMETTI_LINE_CARET_H
#define ZAMETTI_LINE_CARET_H

#include "caret_blink.h"

#include <QColor>

class QLineEdit;
class QPainter;

namespace zametti {

// Разбудить мигание (набор, ход каретки, приход фокуса) и уложить спать.
void wakeLineCaret(CaretBlink& blink, const QLineEdit& field);
// Нарисовать каретку поля поверх штатной. rect — место каретки в координатах
// поля (QLineEdit::cursorRect()), backdrop — цвет заливки поля.
void paintLineCaret(QPainter& painter, const QRect& rect, const CaretBlink& blink,
                    bool focused, const QColor& backdrop);

}  // namespace zametti

#endif  // ZAMETTI_LINE_CARET_H
