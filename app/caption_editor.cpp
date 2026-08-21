#include "caption_editor.h"

#include "settings.h"

namespace zametti {

CaptionEditor::CaptionEditor(const QString& current, QWidget* parent) : LineField(parent) {
    setText(current);
    // Каретка в конец, а не всё выделено: подпись обычно дописывают или
    // подправляют, а не заменяют целиком; заменить — Ctrl+A.
    setCursorPosition(-1);
    // Под полем — страница, ею и закрываем нарисованное (прежнюю подпись).
    setBackdrop(settings().style().pageBackground());
    QPalette colours = palette();
    colours.setColor(QPalette::Base, backdrop());
    colours.setColor(QPalette::Text, settings().style().imageCaptionColor());
    setPalette(colours);
    setAttribute(Qt::WA_MacShowFocusRect, false);
}

}  // namespace zametti
