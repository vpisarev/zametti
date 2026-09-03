#include "caret_blink.h"

#include <cmath>

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

QRectF caretBar(const QRect& cursor, qreal width, qreal scale, qreal dpr) {
    if (!(dpr > 0.0)) dpr = 1.0;
    // Сначала целые логические (как на экране без масштаба), потом — целые
    // физические: так на ретине полоса та же, что на Linux, а не на полпикселя
    // толще (2,4 → 2 логических → 4 физических, а не round(4,8) = 5).
    const qreal physical = qMax(1.0, std::round(caretPixelWidth(width, scale) * dpr));
    const qreal left = std::round(cursor.left() * dpr) / dpr;
    return QRectF(left, cursor.top(), physical / dpr, cursor.height());
}

}  // namespace zametti
