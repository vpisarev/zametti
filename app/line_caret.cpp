#include "line_caret.h"

#include "settings.h"

#include <QLineEdit>
#include <QPainter>

namespace zametti {

void wakeLineCaret(CaretBlink& blink, const QLineEdit& field) {
    blink.wake(field.hasFocus() && !field.isReadOnly());
}

void paintLineCaret(QPainter& painter, const QRect& rect, const CaretBlink& blink, bool focused,
                    const QColor& backdrop) {
    if (!focused) return;
    // Ширина — та же, что в заметке; масштаб полю не нужен: оно и так стоит в
    // кегле того, что правит (подпись, имя языка).
    const int width = caretPixelWidth(settings().style().caretWidth(), 1.0);
    // МЕСТО КАРЕТКИ — СЕРЕДИНА ЭТОГО ПРЯМОУГОЛЬНИКА, А НЕ ЕГО ЛЕВЫЙ КРАЙ: Qt
    // отдаёт под каретку QLineEdit полосу шириной десять пикселей, посаженную
    // серединой на позицию набора (rectForPos: cix - 5, ширина 10). Тем же
    // знанием живёт подсказка языка в lang_editor.cpp — там на нём обожглись.
    QRect at(rect.center().x(), rect.top(), width, rect.height());
    // Погасла — затираем место штатной каретки заливкой поля: погасить её
    // самому Qt не даёт, и она мигала бы своим ритмом сквозь нашу.
    painter.fillRect(at, blink.on() ? settings().style().caretColor() : backdrop);
}

}  // namespace zametti
