// Правка подписи картинки прямо под снимком.
//
// Та же механика, что у языка блока кода (lang_editor.h): поле встаёт на место
// подписи, Enter принимает, Esc и уход фокуса отменяют. Пустая подпись
// законна — картинка остаётся картинкой, а в файл уходит «![](путь)».
//
// Спрятанная подпись («~IMG_1234») в поле видна целиком, вместе со знаком:
// человек видит, ПОЧЕМУ её не видно под снимком, и убирает знак, если хочет
// её вернуть.

#ifndef ZAMETTI_CAPTION_EDITOR_H
#define ZAMETTI_CAPTION_EDITOR_H

#include "line_field.h"

#include <QString>

namespace zametti {

// Каретка, заливка, однострочность, Enter/Esc — у LineField (там же довод, по
// которому поле сделано на QPlainTextEdit, а не на QLineEdit).
class CaptionEditor : public LineField {
    Q_OBJECT

public:
    CaptionEditor(const QString& current, QWidget* parent);
};

}  // namespace zametti

#endif  // ZAMETTI_CAPTION_EDITOR_H
