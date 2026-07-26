#include "checkbox_object.h"

#include <QFontMetricsF>
#include <QPainter>
#include <QPainterPath>
#include <QTextCharFormat>

namespace zametti {
namespace {

constexpr qreal kPenWidth = 1.4;
constexpr qreal kCornerRadius = 2.5;

// Оптическая поправка. Геометрически рамка уже совпадает с чернилами букв, но
// читается чуть низкой: у сплошного прямоугольника вся масса распределена
// равномерно, а у строчных букв она собрана выше — хвост "y" тонкий и лёгкий.
// Доля от высоты, а не пиксели: должна пережить смену кегля.
constexpr qreal kOpticalRise = 0.11;

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
    const qreal side = inkExtent(format).height();
    return QSizeF(side, side);
}

void CheckboxObject::drawObject(QPainter* painter, const QRectF& rect, QTextDocument* doc,
                                int posInDocument, const QTextFormat& format) {
    (void)doc;
    (void)posInDocument;

    const bool checked = format.property(CheckedProperty).toBool();
    const QColor color = format.foreground().style() != Qt::NoBrush
                             ? format.foreground().color()
                             : painter->pen().color();

    // Qt ставит основание объекта на базовую линию, а хвост "y" уходит ниже неё.
    // Поэтому рамку сдвигаем вниз ровно на глубину этого хвоста.
    const QRectF ink = inkExtent(format);
    QRectF box(rect.left(), rect.top() + ink.bottom() - ink.height() * kOpticalRise,
               ink.height(), ink.height());
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
