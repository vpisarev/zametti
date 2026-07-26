#include "checkbox_object.h"

#include <QFontMetricsF>
#include <QPainter>
#include <QPainterPath>
#include <QTextCharFormat>

namespace zametti {
namespace {

// Доля от высоты прописной буквы. Чуть больше единицы: рамка должна читаться
// как самостоятельный элемент, а не как ещё одна буква в строке.
constexpr qreal kBoxOfCapHeight = 1.42;
constexpr qreal kPenWidth = 1.4;
constexpr qreal kCornerRadius = 2.5;

qreal boxSide(const QTextFormat& format) {
    const QFontMetricsF metrics(format.toCharFormat().font());
    return metrics.capHeight() * kBoxOfCapHeight;
}

}  // namespace

QSizeF CheckboxObject::intrinsicSize(QTextDocument* doc, int posInDocument,
                                     const QTextFormat& format) {
    (void)doc;
    (void)posInDocument;
    const qreal side = boxSide(format);
    // Qt ставит объект основанием на базовую линию. Небольшой запас снизу
    // опускает рамку так, чтобы её середина совпала с оптическим центром строки.
    return QSizeF(side, side + kPenWidth);
}

void CheckboxObject::drawObject(QPainter* painter, const QRectF& rect, QTextDocument* doc,
                                int posInDocument, const QTextFormat& format) {
    (void)doc;
    (void)posInDocument;

    const bool checked = format.property(CheckedProperty).toBool();
    const QColor color = format.foreground().style() != Qt::NoBrush
                             ? format.foreground().color()
                             : painter->pen().color();

    const qreal side = boxSide(format);
    QRectF box(rect.left(), rect.top(), side, side);
    box.adjust(kPenWidth / 2, kPenWidth / 2, -kPenWidth / 2, -kPenWidth / 2);

    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);

    QPen pen(color, kPenWidth);
    pen.setJoinStyle(Qt::RoundJoin);
    painter->setPen(pen);

    if (checked) {
        painter->setBrush(color);
        painter->drawRoundedRect(box, kCornerRadius, kCornerRadius);

        // Галочка рисуется на заливке, поэтому цветом фона страницы.
        QPen tick(Qt::white, kPenWidth * 1.15);
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
        painter->drawRoundedRect(box, kCornerRadius, kCornerRadius);
    }

    painter->restore();
}

}  // namespace zametti
