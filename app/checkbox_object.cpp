#include "checkbox_object.h"

#include "appearance.h"

#include <QFontMetricsF>
#include <QPainter>
#include <QPainterPath>
#include <QTextCharFormat>

namespace zametti {
namespace {

using namespace zametti::appearance;

// Рамка занимает по высоте ровно то же, что и строчные буквы с выносными
// элементами: от хвоста "y" до верхушки "i". Абстрактные метрики шрифта
// (ascent/descent) для этого не годятся — они описывают кегельную площадку с
// запасом, и рамка по ним встаёт заметно выше текста. Берём фактические
// чернила букв.
QRectF inkExtent(const QTextFormat& format) {
    const QFontMetricsF metrics(format.toCharFormat().font());
    return metrics.tightBoundingRect(QStringLiteral("iy"));
}

}  // namespace

QSizeF CheckboxObject::intrinsicSize(QTextDocument* doc, int posInDocument,
                                     const QTextFormat& format) {
    (void)doc;
    (void)posInDocument;
    // Запас снизу: с AlignMiddle Qt центрирует прямоугольник по строке, и запас
    // опускает рамку к базовой линии, оставляя её внутри прямоугольника.
    const qreal side = inkExtent(format).height();
    return QSizeF(side, side * (1.0 + kCheckboxOpticalRise));
}

void CheckboxObject::drawObject(QPainter* painter, const QRectF& rect, QTextDocument* doc,
                                int posInDocument, const QTextFormat& format) {
    (void)doc;
    (void)posInDocument;

    const bool checked = format.property(CheckedProperty).toBool();
    const QColor color = checked ? kCheckboxCheckedColor : kCheckboxUncheckedColor;

    // Рисуем строго внутри выданного прямоугольника: всё, что выйдет за его
    // пределы, выделение не закрасит, и снизу останется яркая полоса.
    const qreal side = inkExtent(format).height();
    QRectF box(rect.left(), rect.top() + rect.height() - side, side, side);
    box.adjust(kCheckboxPenWidth / 2, kCheckboxPenWidth / 2, -kCheckboxPenWidth / 2, -kCheckboxPenWidth / 2);

    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);

    QPen pen(color, kCheckboxPenWidth);
    pen.setJoinStyle(Qt::RoundJoin);
    painter->setPen(pen);

    if (checked) {
        painter->setBrush(color);
        painter->drawRoundedRect(box, kCheckboxCornerRadius, kCheckboxCornerRadius);

        // Галочка лежит на заливке, поэтому она белая, а не цвета страницы:
        // под выделением фон страницы меняется, а заливка рамки — нет.
        QPen tick(kCheckboxTickColor, kCheckboxPenWidth * 1.15);
        tick.setCapStyle(Qt::RoundCap);
        tick.setJoinStyle(Qt::RoundJoin);
        painter->setPen(tick);

        QPainterPath path;
        path.moveTo(box.left() + box.width() * 0.24, box.top() + box.height() * 0.52);
        path.lineTo(box.left() + box.width() * 0.43, box.top() + box.height() * 0.72);
        path.lineTo(box.left() + box.width() * 0.78, box.top() + box.height() * 0.28);
        painter->drawPath(path);
    } else {
        painter->setBrush(Qt::NoBrush);
        painter->drawRoundedRect(box, kCheckboxCornerRadius, kCheckboxCornerRadius);
    }

    painter->restore();
}

}  // namespace zametti
