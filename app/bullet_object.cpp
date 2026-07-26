#include "bullet_object.h"

#include "settings.h"

#include <QFontMetricsF>
#include <QPainter>
#include <QTextCharFormat>

#include <algorithm>

namespace zametti {
namespace {

// Всё считается от высоты строчных: буллет должен стоять на их средней линии,
// а она у любого кегля и любой гарнитуры именно там.
qreal xHeightOf(const QTextFormat& format) {
    return QFontMetricsF(format.toCharFormat().font()).xHeight();
}

qreal diameterOf(const QTextFormat& format) {
    return xHeightOf(format) * appearance().bulletDiameter;
}

}  // namespace

QSizeF BulletObject::intrinsicSize(QTextDocument* doc, int posInDocument,
                                   const QTextFormat& format) {
    (void)doc;
    (void)posInDocument;
    // Qt ставит основание объекта на базовую линию, поэтому в высоту берём
    // с запасом: кружок стоит выше неё, и рисовать за пределы прямоугольника
    // нельзя — то, что выйдет наружу, не закрасится выделением.
    return QSizeF(diameterOf(format), xHeightOf(format) + diameterOf(format));
}

void BulletObject::drawObject(QPainter* painter, const QRectF& rect, QTextDocument* doc,
                              int posInDocument, const QTextFormat& format) {
    (void)doc;
    (void)posInDocument;

    const qreal xHeight = xHeightOf(format);
    const qreal diameter = diameterOf(format);

    // Середина строчных лежит на половине их высоты над базовой линией;
    // положительная поправка поднимает кружок, отрицательная опускает.
    const qreal aboveBaseline =
        std::clamp(xHeight / 2 + appearance().bulletRise * xHeight, diameter / 2,
                   rect.height() - diameter / 2);
    const QPointF center(rect.left() + diameter / 2, rect.bottom() - aboveBaseline);

    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);
    painter->setPen(Qt::NoPen);
    painter->setBrush(appearance().bulletColor);
    painter->drawEllipse(center, diameter / 2, diameter / 2);
    painter->restore();
}

}  // namespace zametti
