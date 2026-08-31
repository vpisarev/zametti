#include "marker.h"

#include "document_builder.h"
#include "settings.h"

#include <QFontMetricsF>
#include <QPainter>
#include <QPainterPath>
#include <QAbstractTextDocumentLayout>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextLayout>
#include <QTextLine>

namespace zametti {
namespace {

bool drawnCheckbox(MarkerStyle style, const ZDocStyle& look) {
    return style.marker == Marker::Task && look.checkboxStyle() == CheckboxStyle::Drawn;
}

bool drawnBullet(MarkerStyle style, const ZDocStyle& look) {
    return style.marker == Marker::Bullet && look.bulletStyle() == BulletStyle::Drawn;
}

// Буквенный номер: a..z, aa..zz, aaa... — биективная 26-ричная запись,
// как колонки в таблицах.
QString lettersFor(int ordinal) {
    QString out;
    for (int n = ordinal; n > 0; n /= 26) {
        --n;
        out.prepend(QChar(QLatin1Char(char('a' + n % 26))));
    }
    return out;
}

}  // namespace

// Знак маркера — для тех начертаний, где он берётся из шрифта. У нарисованных
// (кружок, рамка) знака нет. Нумерованный меняет вид по вложенности, как
// буллет меняет фигуру: 1. 2. 3. → a. b. c. → 1) 2) 3) — и снова по кругу.
QString markerText(MarkerStyle style, int ordinal, int level, const ZDocStyle& look) {
    switch (style.marker) {
        case Marker::Bullet:
            return drawnBullet(style, look) ? QString() : look.bulletGlyph();
        case Marker::Ordered:
            switch (qMax(0, level) % 3) {
                case 1:  return lettersFor(ordinal) + QStringLiteral(".");
                case 2:  return QString::number(ordinal) + QStringLiteral(")");
                default: return QString::number(ordinal) + QStringLiteral(".");
            }
        case Marker::Task:
            switch (look.checkboxStyle()) {
                case CheckboxStyle::Glyph:
                    return style.checked ? QStringLiteral("☑") : QStringLiteral("☐");
                case CheckboxStyle::Ascii:
                    return style.checked ? QStringLiteral("[x]") : QStringLiteral("[ ]");
                case CheckboxStyle::Drawn:
                    return QString();
            }
            return QString();
    }
    return QString();
}

namespace {

QFont markerFont(MarkerStyle style, const QFont& base, const ZDocStyle& look) {
    QFont font = base;
    if (style.marker == Marker::Bullet && look.bulletStyle() == BulletStyle::Glyph)
        font.setPointSizeF(base.pointSizeF() * look.bulletScale());
    // Цифры и буквы номера — ОСНОВНЫМ ШРИФТОМ ЗАМЕТКИ, а не тем, что достался
    // base (решение владельца 31.08): в base может приехать шрифт другого
    // режима, и гарнитуру номера нельзя отпускать на самотёк. Кегль остаётся
    // от base — размер идёт за текстом и зумом.
    if (style.marker == Marker::Ordered)
        font.setFamilies({QString(look.fontFamily())});
    if (style.marker == Marker::Task && look.checkboxStyle() == CheckboxStyle::Glyph) {
        font.setFamilies({QString(look.symbolFamily()), QString(look.fontFamily())});
        font.setPointSizeF(base.pointSizeF() * look.checkboxGlyphScale());
    }
    return font;
}

// Метрики базового шрифта, считанные один раз на шрифт.
//
// Зовут их на КАЖДЫЙ маркер в кадре, а tightBoundingRect строит контуры глифов
// — самая дорогая из трёх. Шрифтов за кадр теперь ДВА, и оба постоянны:
// зумленный шрифт документа (глиф) и незумленный шрифт вёрстки (зазор якоря,
// см. anchorOf). Отсюда кэш на две записи: однозаписный промахивался бы на
// каждом маркере, а сравнить QFont дёшево, посчитать заново — нет.
struct BaseMetrics {
    qreal charUnit = 0;      // ширина "A"
    qreal xHeight = 0;
    qreal checkboxSide = 0;  // высота чернил строчных с выносными
};

const BaseMetrics& metricsOf(const QFont& base) {
    struct Slot {
        QFont font;
        BaseMetrics metrics;
        bool valid = false;
    };
    // Имя kept, не slots: slots — макрос Qt, и массив под ним не объявить.
    static Slot kept[2];
    static int last = 0;
    for (const Slot& slot : kept)
        if (slot.valid && slot.font == base) return slot.metrics;

    Slot& fresh = kept[last ^= 1];
    const QFontMetricsF metrics(base);
    fresh.metrics.charUnit = metrics.horizontalAdvance(QLatin1Char('A'));
    fresh.metrics.xHeight = metrics.xHeight();
    // Ровно высота чернил строчных с выносными элементами, от хвоста "y" до
    // верхушки "i". Абстрактные ascent/descent для этого не годятся, они
    // описывают кегельную площадку с запасом.
    fresh.metrics.checkboxSide =
        metrics.tightBoundingRect(QStringLiteral("iy")).height();
    fresh.font = base;
    fresh.valid = true;
    return fresh.metrics;
}

// Зазор от маркера до текста. В ширинах "A", а не в пробелах: у пропорциональных
// гарнитур пробел вдвое уже буквы, и колонка на нём выходила бы вплотную.
qreal gapFor(MarkerStyle style, const QFont& base, const ZDocStyle& look) {
    const qreal unit = metricsOf(base).charUnit;
    switch (style.marker) {
        case Marker::Task:    return look.checkboxTextGap() * unit;
        case Marker::Ordered: return look.orderedTextGap() * unit;
        case Marker::Bullet:  return look.bulletTextGap() * unit;
    }
    return look.bulletTextGap() * unit;
}

qreal checkboxSide(const QFont& base) { return metricsOf(base).checkboxSide; }

qreal glyphWidth(MarkerStyle style, int ordinal, int level, const QFont& base,
                 const ZDocStyle& look) {
    if (drawnCheckbox(style, look)) return checkboxSide(base);
    if (drawnBullet(style, look)) return metricsOf(base).xHeight * look.bulletDiameter();
    return QFontMetricsF(markerFont(style, base, look))
        .horizontalAdvance(markerText(style, ordinal, level, look));
}

// Опора маркера в координатах документа: правый край его колонки и базовая
// линия первой строки блока. Маркер прижимается к правому краю — тогда у
// нумерованного списка точки стоят одна под другой независимо от числа цифр.
struct Anchor {
    qreal right = 0;
    qreal baseline = 0;
    bool valid = false;
};

Anchor anchorOf(const QTextBlock& block, MarkerStyle style, const QFont&,
                const ZDocStyle& look) {
    const QTextLayout* layout = block.layout();
    if (layout == nullptr || layout->lineCount() == 0) return {};
    const QTextLine line = layout->lineAt(0);
    const QPointF origin = layout->position();
    // Позиция раскладки — левый край содержимого фрейма; левое поле блока в неё
    // не входит, его надо прибавить.
    const qreal textLeft = origin.x() + block.blockFormat().leftMargin();
    // ЗАЗОР — НЕЗУМЛЕННЫМ ШРИФТОМ ВЁРСТКИ, тем же, каким applyListGeometry
    // испёк leftMargin: левый край текста при зуме неподвижен в пикселях (поля
    // блоков при зуме не переписывают — каждая запись формата была бы шагом
    // отмены, см. known_bugs.md), и якорь маркера обязан стоять так же. Зазор
    // зумленным шрифтом — это и был съезд маркера на gap·(z−1), у каждого рода
    // свой (жалоба владельца 31.08). Цена неподвижности, названная вслух:
    // при z≠1 зазор — константа в пикселях, а не доля кегля.
    const qreal gap = gapFor(style, layoutBaseFont(look), look);
    return {textLeft - gap, origin.y() + line.y() + line.ascent(), true};
}

// Отсечка роста глифа у левого края колонки документа — приём plateScale
// (note_view.cpp): глиф растёт с зумом ВЛЕВО от прибитого правого края, там
// пустота собственного отступа пункта, но у пункта верхнего уровня в узком
// окне пустота кончается на x=0 — дальше рост упирается.
qreal glyphScaleFor(MarkerStyle style, int ordinal, int level, const QFont& base,
                    const ZDocStyle& look, qreal anchorRight) {
    const qreal wanted = glyphWidth(style, ordinal, level, base, look);
    const qreal available = qMax(0.0, anchorRight);
    if (wanted <= available || wanted <= 0) return 1.0;
    qreal scale = available / wanted;
    // Шрифтовые метрики не строго линейны по кеглю — одна поправка по факту.
    if (!drawnCheckbox(style, look) && !drawnBullet(style, look)) {
        QFont shrunk = markerFont(style, base, look);
        shrunk.setPointSizeF(qMax(1.0, shrunk.pointSizeF() * scale));
        const qreal again = QFontMetricsF(shrunk).horizontalAdvance(
            markerText(style, ordinal, level, look));
        if (again > available && again > 0) scale *= available / again;
    }
    return scale;
}

// Шрифт знакового маркера с уже применённой отсечкой. Общий и отрисовке, и
// markerBoxOf: двух копий геометрии быть не должно.
QFont scaledMarkerFont(MarkerStyle style, int ordinal, int level, const QFont& base,
                       const ZDocStyle& look, qreal anchorRight) {
    QFont font = markerFont(style, base, look);
    const qreal scale = glyphScaleFor(style, ordinal, level, base, look, anchorRight);
    if (scale < 1.0) font.setPointSizeF(qMax(1.0, font.pointSizeF() * scale));
    return font;
}

// Диаметр рисованного буллета с отсечкой — тот же владелец, те же два клиента.
qreal bulletDiameterFor(MarkerStyle style, int level, const QFont& base,
                        const ZDocStyle& look, qreal anchorRight) {
    return metricsOf(base).xHeight * look.bulletDiameter() *
           glyphScaleFor(style, 0, level, base, look, anchorRight);
}

// Буллет рисуется в круге заданного диаметра — какой бы ни была фигура. Место
// под маркер от фигуры не зависит, иначе текст на разных уровнях вставал бы по
// разным колонкам без всякой на то причины.
void paintBullet(QPainter& painter, const QPointF& center, qreal diameter,
                 BulletShape shape, const ZDocStyle& look) {
    const QColor color = look.bulletColor();
    switch (shape) {
        case BulletShape::Disc:
            painter.setPen(Qt::NoPen);
            painter.setBrush(color);
            painter.drawEllipse(center, diameter / 2, diameter / 2);
            return;
        case BulletShape::Circle: {
            // Обводка идёт по средней линии, поэтому радиус берём на полтолщины
            // меньше: внешний край кружка совпадает со сплошным того же размера.
            const qreal pen = qMax(0.5, diameter * look.bulletStrokeWidth());
            const qreal radius = (diameter - pen) / 2;
            painter.setPen(QPen(color, pen));
            painter.setBrush(Qt::NoBrush);
            painter.drawEllipse(center, radius, radius);
            return;
        }
        case BulletShape::Square: {
            const qreal side = diameter * look.bulletSquareSide();
            painter.setPen(Qt::NoPen);
            painter.setBrush(color);
            painter.drawRect(QRectF(center.x() - side / 2, center.y() - side / 2, side, side));
            return;
        }
    }
}

void paintCheckbox(QPainter& painter, const QRectF& rect, bool checked, const ZDocStyle& look) {
    const QColor color =
        checked ? look.checkboxCheckedColor() : look.checkboxUncheckedColor();
    const qreal pen = look.checkboxPenWidth();
    const QRectF box = rect.adjusted(pen / 2, pen / 2, -pen / 2, -pen / 2);

    QPen outline(color, pen);
    outline.setJoinStyle(Qt::RoundJoin);
    painter.setPen(outline);
    painter.setBrush(checked ? QBrush(color) : QBrush(Qt::NoBrush));
    painter.drawRoundedRect(box, look.checkboxCornerRadius(),
                            look.checkboxCornerRadius());
    if (!checked) return;

    // Галочка лежит на заливке, поэтому она белая, а не цвета страницы: под
    // выделением фон страницы меняется, а заливка рамки — нет.
    QPen tick(look.checkboxTickColor(), pen * 1.15);
    tick.setCapStyle(Qt::RoundCap);
    tick.setJoinStyle(Qt::RoundJoin);
    painter.setPen(tick);

    QPainterPath path;
    path.moveTo(box.left() + box.width() * 0.24, box.top() + box.height() * 0.52);
    path.lineTo(box.left() + box.width() * 0.43, box.top() + box.height() * 0.72);
    path.lineTo(box.left() + box.width() * 0.78, box.top() + box.height() * 0.28);
    painter.drawPath(path);
}

}  // namespace

BulletShape bulletShapeFor(int level, const ZDocStyle& look) {
    const std::vector<BulletShape>& shapes = look.bulletShapes();
    if (shapes.empty()) return BulletShape::Disc;
    return shapes[size_t(qBound(0, level, int(shapes.size()) - 1))];
}

qreal markerColumn(MarkerStyle style, int ordinal, int level, const QFont& base,
                   const ZDocStyle& look) {
    return glyphWidth(style, ordinal, level, base, look) + gapFor(style, base, look);
}

QRectF checkboxRect(const QTextBlock& block, const QFont& base) {
    const ZDocStyle& look = styleOf(*block.document());
    const MarkerStyle style = markerOf(block);
    if (!isListBlock(block) || !drawnCheckbox(style, look)) return {};
    const Anchor anchor = anchorOf(block, style, base, look);
    if (!anchor.valid) return {};

    const QRectF ink = QFontMetricsF(base).tightBoundingRect(QStringLiteral("iy"));
    // Рамка растёт с зумом (шрифт base), правый край прибит якорем; у края
    // окна рост отсекается той же отсечкой, что у остальных глифов.
    const qreal side =
        ink.height() * glyphScaleFor(style, 0, 0, base, look, anchor.right);
    // ink.top() отрицателен: столько чернил выше базовой линии. Поправка со
    // знаком: больше нуля поднимает рамку.
    const qreal top = anchor.baseline + ink.top() - look.checkboxOpticalRise() * side;
    return QRectF(anchor.right - side, top, side, side);
}

QTextBlock blockAtCheckbox(const QTextDocument& doc, const QPointF& point,
                           const QFont& base) {
    const QAbstractTextDocumentLayout* layout = doc.documentLayout();
    // От первого блока, попадающего в строку с этой точкой: обходить документ с
    // начала незачем.
    const int at = layout->hitTest(QPointF(0, point.y()), Qt::FuzzyHit);
    for (QTextBlock block = doc.findBlock(at); block.isValid(); block = block.next()) {
        const QRectF rect = layout->blockBoundingRect(block);
        if (rect.top() > point.y()) break;
        if (rect.bottom() < point.y()) continue;
        const QRectF box = checkboxRect(block, base);
        // Промахнуться по рамке легко, поэтому попадание считаем с запасом в
        // половину её стороны со всех сторон.
        if (!box.isNull() && box.adjusted(-box.width() / 2, -box.height() / 2,
                                          box.width() / 2, box.height() / 2)
                                 .contains(point))
            return block;
    }
    return QTextBlock();
}

void paintMarker(QPainter& painter, const QTextBlock& block, const QFont& base) {
    const ZDocStyle& look = styleOf(*block.document());
    if (!isListBlock(block)) return;
    const MarkerStyle style = markerOf(block);
    const Anchor anchor = anchorOf(block, style, base, look);
    if (!anchor.valid) return;

    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);

    if (drawnCheckbox(style, look)) {
        paintCheckbox(painter, checkboxRect(block, base), style.checked, look);
    } else if (drawnBullet(style, look)) {
        const qreal xHeight = metricsOf(base).xHeight;
        const qreal diameter =
            bulletDiameterFor(style, levelOf(block), base, look, anchor.right);
        // Кружок стоит на средней линии строчных: она у любой гарнитуры именно
        // там, где глаз ждёт буллет.
        const QPointF center(anchor.right - diameter / 2,
                             anchor.baseline - xHeight / 2 -
                                 look.bulletRise() * xHeight);
        paintBullet(painter, center, diameter, bulletShapeFor(levelOf(block), look), look);
    } else {
        const QFont font = scaledMarkerFont(style, ordinalOf(block), levelOf(block),
                                            base, look, anchor.right);
        const QString text = markerText(style, ordinalOf(block), levelOf(block), look);
        const QColor color =
            style.marker == Marker::Task
                ? (style.checked ? look.checkboxCheckedColor()
                                 : look.checkboxUncheckedColor())
            : style.marker == Marker::Ordered ? look.orderedColor()
                                              : look.bulletColor();
        // Поправка по вертикали — от высоты строчных основного шрифта, а не
        // маркерного: маркер должен двигаться относительно текста строки.
        qreal rise = 0;
        if (style.marker == Marker::Ordered) rise = look.orderedRise();
        else if (style.marker == Marker::Bullet) rise = look.bulletRise();
        painter.setFont(font);
        painter.setPen(color);
        painter.drawText(QPointF(anchor.right - QFontMetricsF(font).horizontalAdvance(text),
                                 anchor.baseline - rise * metricsOf(base).xHeight),
                         text);
    }

    painter.restore();
}

QFont markerFontFor(MarkerStyle style, const QFont& base, const ZDocStyle& look) {
    return markerFont(style, base, look);
}

QRectF markerBoxOf(const QTextBlock& block, const QFont& base) {
    const ZDocStyle& look = styleOf(*block.document());
    if (!isListBlock(block)) return {};
    const MarkerStyle style = markerOf(block);
    if (drawnCheckbox(style, look)) return checkboxRect(block, base);
    const Anchor anchor = anchorOf(block, style, base, look);
    if (!anchor.valid) return {};

    if (drawnBullet(style, look)) {
        const qreal xHeight = metricsOf(base).xHeight;
        const qreal diameter =
            bulletDiameterFor(style, levelOf(block), base, look, anchor.right);
        const qreal centerY =
            anchor.baseline - xHeight / 2 - look.bulletRise() * xHeight;
        return QRectF(anchor.right - diameter, centerY - diameter / 2, diameter,
                      diameter);
    }
    const QFont font = scaledMarkerFont(style, ordinalOf(block), levelOf(block),
                                        base, look, anchor.right);
    const QFontMetricsF metrics(font);
    const QString text = markerText(style, ordinalOf(block), levelOf(block), look);
    const qreal width = metrics.horizontalAdvance(text);
    qreal rise = 0;
    if (style.marker == Marker::Ordered) rise = look.orderedRise();
    else if (style.marker == Marker::Bullet) rise = look.bulletRise();
    const qreal baseline = anchor.baseline - rise * metricsOf(base).xHeight;
    return QRectF(anchor.right - width, baseline - metrics.ascent(), width,
                  metrics.ascent() + metrics.descent());
}

void paintDivider(QPainter& painter, const QTextBlock& block, const QRectF& rect, qreal zoom) {
    const ZDocStyle& look = styleOf(*block.document());
    if (isRawBlock(block) || kindOf(block) != Kind::Divider) return;
    painter.save();
    // Прямоугольник блока и так идёт от поля до поля колонки — не во всё окно.
    QPen pen(look.dividerColor());
    pen.setWidthF(qMax(1.0, look.dividerWidth() * zoom));
    pen.setCapStyle(Qt::FlatCap);
    painter.setPen(pen);
    const qreal y = rect.center().y();
    painter.drawLine(QPointF(rect.left(), y), QPointF(rect.right(), y));
    painter.restore();
}

}  // namespace zametti
