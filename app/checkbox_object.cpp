#include "checkbox_object.h"

#include "settings.h"

#include <QFontMetricsF>
#include <QPainter>
#include <QPainterPath>
#include <QTextCharFormat>

namespace zametti {
namespace {

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

qreal CheckboxObject::sideFor(const QFont& font) {
    return QFontMetricsF(font).tightBoundingRect(QStringLiteral("iy")).height();
}

QSizeF CheckboxObject::intrinsicSize(QTextDocument* doc, int posInDocument,
                                     const QTextFormat& format) {
    (void)doc;
    (void)posInDocument;
    // Запас снизу: с AlignMiddle Qt центрирует прямоугольник по строке, и запас
    // опускает рамку к базовой линии, оставляя её внутри прямоугольника.
    const qreal side = inkExtent(format).height();
    return QSizeF(side, side * (1.0 + appearance().checkboxOpticalRise));
}

void CheckboxObject::drawObject(QPainter* painter, const QRectF& rect, QTextDocument* doc,
                                int posInDocument, const QTextFormat& format) {
    (void)doc;
    (void)posInDocument;

    const bool checked = format.property(CheckedProperty).toBool();
    const QColor color = checked ? appearance().checkboxCheckedColor : appearance().checkboxUncheckedColor;

    // Рисуем строго внутри выданного прямоугольника: всё, что выйдет за его
    // пределы, выделение не закрасит, и снизу останется яркая полоса.
    const qreal side = inkExtent(format).height();
    QRectF box(rect.left(), rect.top() + rect.height() - side, side, side);
    box.adjust(appearance().checkboxPenWidth / 2, appearance().checkboxPenWidth / 2, -appearance().checkboxPenWidth / 2, -appearance().checkboxPenWidth / 2);

    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);

    QPen pen(color, appearance().checkboxPenWidth);
    pen.setJoinStyle(Qt::RoundJoin);
    painter->setPen(pen);

    if (checked) {
        painter->setBrush(color);
        painter->drawRoundedRect(box, appearance().checkboxCornerRadius, appearance().checkboxCornerRadius);

        // Галочка лежит на заливке, поэтому она белая, а не цвета страницы:
        // под выделением фон страницы меняется, а заливка рамки — нет.
        QPen tick(appearance().checkboxTickColor, appearance().checkboxPenWidth * 1.15);
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
        painter->drawRoundedRect(box, appearance().checkboxCornerRadius, appearance().checkboxCornerRadius);
    }

    painter->restore();
}

}  // namespace zametti
