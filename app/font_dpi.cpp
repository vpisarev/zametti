#include "font_dpi.h"

#include <QByteArray>
#include <QGuiApplication>
#include <QtGlobal>

namespace zametti {

void prepareFontDpi(int dpi) {
    Q_ASSERT(dpi > 0);
    if (dpi <= 0) return;
    if (qEnvironmentVariableIsEmpty("QT_FONT_DPI")) qputenv("QT_FONT_DPI", QByteArray::number(dpi));
    // Округление вниз до 0,75 и вверх после: множитель 4/3 (96/72) уходит в
    // единицу целиком, а 1,5 стал бы двойкой — оба целые, и в обоих случаях
    // остаток достаётся шрифтам.
    QGuiApplication::setHighDpiScaleFactorRoundingPolicy(
        Qt::HighDpiScaleFactorRoundingPolicy::RoundPreferFloor);
}

}  // namespace zametti
