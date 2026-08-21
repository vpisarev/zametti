#include "caret_blink.h"

#include <QGuiApplication>
#include <QStyleHints>
#include <QtGlobal>

namespace zametti {

CaretBlink::CaretBlink(QObject* parent) : QObject(parent) {
    timer_.setInterval(qMax(250, QGuiApplication::styleHints()->cursorFlashTime() / 2));
    connect(&timer_, &QTimer::timeout, this, [this] {
        on_ = !on_;
        emit phaseChanged();
    });
}

void CaretBlink::wake(bool blink) {
    on_ = true;
    if (blink)
        timer_.start();   // перезапуск: отсчёт фазы — с этого мгновения
    else
        timer_.stop();
}

void CaretBlink::sleep() {
    timer_.stop();
    on_ = false;
}

int caretPixelWidth(qreal width, qreal scale) { return qMax(1, qRound(width * scale)); }

}  // namespace zametti
