#include "object_frame.h"

#include "settings.h"

namespace zametti {

// Вынос: отступ наружу плюс толщина пера — ровно столько занимает уголок за
// краем объекта (см. paintCorners: box раздвигается на out + thick).
qreal ObjectFrame::overhang() {
    const ZSettings& a = settings();
    return qMax(0.0, a.style().imageCornerOffset()) + qMax(0.5, a.style().imageCornerWidth());
}

void ObjectFrame::paintCorners(QPainter& painter, const QRectF& box_) {
    if (box_.isEmpty()) return;
    const ZSettings& a = settings();
    const qreal shortSide = qMin(box_.width(), box_.height());
    // Доля от ПОКАЗАННОГО размера, а не от размера файла: уголки — это про то,
    // что человек видит на экране. Пол — чтобы на маленьком объекте уголок не
    // выродился в точку, потолок — сама короткая сторона: длиннее ему негде.
    const qreal length =
        qMin(shortSide, qMax(shortSide * qMax(0.0, a.style().imageCornerShare()),
                             qreal(a.style().imageCornerMinLength())));
    const qreal thick = qMax(0.5, a.style().imageCornerWidth());
    if (length <= 0.0) return;

    // Каждый уголок — ОДИН многоугольник, а не две линии. Двумя линиями в
    // самом углу выходил заметный артефакт: два прямоугольника накладывались
    // под прямым углом, и стык был виден ступенькой.
    const qreal out = qMax(0.0, a.style().imageCornerOffset());
    const QRectF box = box_.adjusted(-out - thick, -out - thick, out + thick, out + thick);

    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(Qt::NoPen);
    painter.setBrush(a.style().caretColor());
    for (int corner = 0; corner < 4; ++corner) {
        const bool right = corner == 1 || corner == 2;
        const bool bottom = corner >= 2;
        const QPointF at(right ? box.right() : box.left(), bottom ? box.bottom() : box.top());
        const qreal dx = right ? -1.0 : 1.0;
        const qreal dy = bottom ? -1.0 : 1.0;
        const QPointF points[6] = {
            at,
            at + QPointF(dx * length, 0),
            at + QPointF(dx * length, dy * thick),
            at + QPointF(dx * thick, dy * thick),
            at + QPointF(dx * thick, dy * length),
            at + QPointF(0, dy * length),
        };
        painter.drawPolygon(points, 6);
    }
    painter.restore();
}

}  // namespace zametti
