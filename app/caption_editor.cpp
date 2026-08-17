#include "caption_editor.h"

#include "settings.h"

#include <QKeyEvent>
#include <QPainter>

namespace zametti {

CaptionEditor::CaptionEditor(const QString& current, QWidget* parent) : QLineEdit(parent) {
    setFrame(false);
    setText(current);
    // Каретка в конец, а не всё выделено: подпись обычно дописывают или
    // подправляют, а не заменяют целиком; заменить — Ctrl+A.
    setCursorPosition(current.size());
    // ФОН ЗАКРАШИВАЕМ САМИ, в paintEvent (см. довод в lang_editor.cpp: без
    // фрейма палитра Base у поля не рисуется, а autoFillBackground красит
    // чужой ролью). Под полем — страница, ею и закрываем.
    backdrop_ = settings().look().pageBackground();
    QPalette colours = palette();
    colours.setColor(QPalette::Base, backdrop_);
    colours.setColor(QPalette::Text, settings().look().imageCaptionColor());
    setPalette(colours);
    setAttribute(Qt::WA_MacShowFocusRect, false);
}

void CaptionEditor::keyPressEvent(QKeyEvent* event) {
    switch (event->key()) {
        case Qt::Key_Return:
        case Qt::Key_Enter:
            emit accepted(text());
            return;
        case Qt::Key_Escape:
            emit cancelled();
            return;
        default:
            break;
    }
    QLineEdit::keyPressEvent(event);
}

void CaptionEditor::paintEvent(QPaintEvent* event) {
    {
        QPainter under(this);
        under.fillRect(rect(), backdrop_);
    }
    QLineEdit::paintEvent(event);
}

void CaptionEditor::focusOutEvent(QFocusEvent* event) {
    QLineEdit::focusOutEvent(event);
    // Ушли мимо — как Esc: молча применять то, чего человек не подтвердил,
    // нельзя, а оставлять поле висеть поверх текста — тем более.
    emit cancelled();
}

}  // namespace zametti
