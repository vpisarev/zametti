#include "marker.h"

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

bool drawnCheckbox(MarkerStyle style) {
    return style.marker == Marker::Task && appearance().checkboxStyle == CheckboxStyle::Drawn;
}

bool drawnBullet(MarkerStyle style) {
    return style.marker == Marker::Bullet && appearance().bulletStyle == BulletStyle::Drawn;
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
QString markerText(MarkerStyle style, int ordinal, int level) {
    switch (style.marker) {
        case Marker::Bullet:
            return drawnBullet(style) ? QString() : appearance().bulletGlyph;
        case Marker::Ordered:
            switch (qMax(0, level) % 3) {
                case 1:  return lettersFor(ordinal) + QStringLiteral(".");
                case 2:  return QString::number(ordinal) + QStringLiteral(")");
                default: return QString::number(ordinal) + QStringLiteral(".");
            }
        case Marker::Task:
            switch (appearance().checkboxStyle) {
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

QFont markerFont(MarkerStyle style, const QFont& base) {
    QFont font = base;
    if (style.marker == Marker::Bullet && appearance().bulletStyle == BulletStyle::Glyph)
        font.setPointSizeF(base.pointSizeF() * appearance().bulletScale);
    if (style.marker == Marker::Task && appearance().checkboxStyle == CheckboxStyle::Glyph) {
        font.setFamilies({QString(appearance().symbolFamily), QString(appearance().fontFamily)});
        font.setPointSizeF(base.pointSizeF() * appearance().checkboxGlyphScale);
    }
    return font;
}

// Метрики базового шрифта, считанные один раз на шрифт.
//
// Зовут их на КАЖДЫЙ маркер в кадре, а tightBoundingRect строит контуры глифов
// — самая дорогая из трёх. Шрифт же весь кадр один и тот же, и меняется он
// только от зума или настроек. Отсюда кэш на одну запись: сравнить QFont
// дёшево, посчитать заново — нет.
struct BaseMetrics {
    qreal charUnit = 0;      // ширина "A"
    qreal xHeight = 0;
    qreal checkboxSide = 0;  // высота чернил строчных с выносными
};

const BaseMetrics& metricsOf(const QFont& base) {
    static QFont knownFont;
    static BaseMetrics known;
    static bool valid = false;
    if (valid && knownFont == base) return known;

    const QFontMetricsF metrics(base);
    known.charUnit = metrics.horizontalAdvance(QLatin1Char('A'));
    known.xHeight = metrics.xHeight();
    // Ровно высота чернил строчных с выносными элементами, от хвоста "y" до
    // верхушки "i". Абстрактные ascent/descent для этого не годятся, они
    // описывают кегельную площадку с запасом.
    known.checkboxSide = metrics.tightBoundingRect(QStringLiteral("iy")).height();
    knownFont = base;
    valid = true;
    return known;
}

// Зазор от маркера до текста. В ширинах "A", а не в пробелах: у пропорциональных
// гарнитур пробел вдвое уже буквы, и колонка на нём выходила бы вплотную.
qreal gapFor(MarkerStyle style, const QFont& base) {
    const qreal unit = metricsOf(base).charUnit;
    switch (style.marker) {
        case Marker::Task:    return appearance().checkboxTextGap * unit;
        case Marker::Ordered: return appearance().orderedTextGap * unit;
        case Marker::Bullet:  return appearance().bulletTextGap * unit;
    }
    return appearance().bulletTextGap * unit;
}

qreal checkboxSide(const QFont& base) { return metricsOf(base).checkboxSide; }

qreal glyphWidth(MarkerStyle style, int ordinal, int level, const QFont& base) {
    if (drawnCheckbox(style)) return checkboxSide(base);
    if (drawnBullet(style)) return metricsOf(base).xHeight * appearance().bulletDiameter;
    return QFontMetricsF(markerFont(style, base))
        .horizontalAdvance(markerText(style, ordinal, level));
}

// Опора маркера в координатах документа: правый край его колонки и базовая
// линия первой строки блока. Маркер прижимается к правому краю — тогда у
// нумерованного списка точки стоят одна под другой независимо от числа цифр.
struct Anchor {
    qreal right = 0;
    qreal baseline = 0;
    bool valid = false;
};

Anchor anchorOf(const QTextBlock& block, MarkerStyle style, const QFont& base) {
    const QTextLayout* layout = block.layout();
    if (layout == nullptr || layout->lineCount() == 0) return {};
    const QTextLine line = layout->lineAt(0);
    const QPointF origin = layout->position();
    // Позиция раскладки — левый край содержимого фрейма; левое поле блока в неё
    // не входит, его надо прибавить.
    const qreal textLeft = origin.x() + block.blockFormat().leftMargin();
    return {textLeft - gapFor(style, base), origin.y() + line.y() + line.ascent(), true};
}

// Буллет рисуется в круге заданного диаметра — какой бы ни была фигура. Место
// под маркер от фигуры не зависит, иначе текст на разных уровнях вставал бы по
// разным колонкам без всякой на то причины.
void paintBullet(QPainter& painter, const QPointF& center, qreal diameter,
                 BulletShape shape) {
    const QColor color = appearance().bulletColor;
    switch (shape) {
        case BulletShape::Disc:
            painter.setPen(Qt::NoPen);
            painter.setBrush(color);
            painter.drawEllipse(center, diameter / 2, diameter / 2);
            return;
        case BulletShape::Circle: {
            // Обводка идёт по средней линии, поэтому радиус берём на полтолщины
            // меньше: внешний край кружка совпадает со сплошным того же размера.
            const qreal pen = qMax(0.5, diameter * appearance().bulletStrokeWidth);
            const qreal radius = (diameter - pen) / 2;
            painter.setPen(QPen(color, pen));
            painter.setBrush(Qt::NoBrush);
            painter.drawEllipse(center, radius, radius);
            return;
        }
        case BulletShape::Square: {
            const qreal side = diameter * appearance().bulletSquareSide;
            painter.setPen(Qt::NoPen);
            painter.setBrush(color);
            painter.drawRect(QRectF(center.x() - side / 2, center.y() - side / 2, side, side));
            return;
        }
    }
}

void paintCheckbox(QPainter& painter, const QRectF& rect, bool checked) {
    const QColor color =
        checked ? appearance().checkboxCheckedColor : appearance().checkboxUncheckedColor;
    const qreal pen = appearance().checkboxPenWidth;
    const QRectF box = rect.adjusted(pen / 2, pen / 2, -pen / 2, -pen / 2);

    QPen outline(color, pen);
    outline.setJoinStyle(Qt::RoundJoin);
    painter.setPen(outline);
    painter.setBrush(checked ? QBrush(color) : QBrush(Qt::NoBrush));
    painter.drawRoundedRect(box, appearance().checkboxCornerRadius,
                            appearance().checkboxCornerRadius);
    if (!checked) return;

    // Галочка лежит на заливке, поэтому она белая, а не цвета страницы: под
    // выделением фон страницы меняется, а заливка рамки — нет.
    QPen tick(appearance().checkboxTickColor, pen * 1.15);
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

BulletShape bulletShapeFor(int level) {
    const std::vector<BulletShape>& shapes = appearance().bulletShapes;
    if (shapes.empty()) return BulletShape::Disc;
    return shapes[size_t(qBound(0, level, int(shapes.size()) - 1))];
}

qreal markerColumn(MarkerStyle style, int ordinal, int level, const QFont& base) {
    return glyphWidth(style, ordinal, level, base) + gapFor(style, base);
}

QRectF checkboxRect(const QTextBlock& block, const QFont& base) {
    const MarkerStyle style = markerOf(block);
    if (!isListBlock(block) || !drawnCheckbox(style)) return {};
    const Anchor anchor = anchorOf(block, style, base);
    if (!anchor.valid) return {};

    const QRectF ink = QFontMetricsF(base).tightBoundingRect(QStringLiteral("iy"));
    const qreal side = ink.height();
    // ink.top() отрицателен: столько чернил выше базовой линии. Поправка со
    // знаком: больше нуля поднимает рамку.
    const qreal top = anchor.baseline + ink.top() - appearance().checkboxOpticalRise * side;
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
    if (!isListBlock(block)) return;
    const MarkerStyle style = markerOf(block);
    const Anchor anchor = anchorOf(block, style, base);
    if (!anchor.valid) return;

    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);

    if (drawnCheckbox(style)) {
        paintCheckbox(painter, checkboxRect(block, base), style.checked);
    } else if (drawnBullet(style)) {
        const qreal xHeight = metricsOf(base).xHeight;
        const qreal diameter = xHeight * appearance().bulletDiameter;
        // Кружок стоит на средней линии строчных: она у любой гарнитуры именно
        // там, где глаз ждёт буллет.
        const QPointF center(anchor.right - diameter / 2,
                             anchor.baseline - xHeight / 2 -
                                 appearance().bulletRise * xHeight);
        paintBullet(painter, center, diameter, bulletShapeFor(levelOf(block)));
    } else {
        const QFont font = markerFont(style, base);
        const QString text = markerText(style, ordinalOf(block), levelOf(block));
        const QColor color =
            style.marker == Marker::Task
                ? (style.checked ? appearance().checkboxCheckedColor
                                 : appearance().checkboxUncheckedColor)
            : style.marker == Marker::Ordered ? appearance().orderedColor
                                              : appearance().bulletColor;
        // Поправка по вертикали — от высоты строчных основного шрифта, а не
        // маркерного: маркер должен двигаться относительно текста строки.
        qreal rise = 0;
        if (style.marker == Marker::Ordered) rise = appearance().orderedRise;
        else if (style.marker == Marker::Bullet) rise = appearance().bulletRise;
        painter.setFont(font);
        painter.setPen(color);
        painter.drawText(QPointF(anchor.right - QFontMetricsF(font).horizontalAdvance(text),
                                 anchor.baseline - rise * metricsOf(base).xHeight),
                         text);
    }

    painter.restore();
}

void paintDivider(QPainter& painter, const QTextBlock& block, const QRectF& rect, qreal zoom) {
    if (isRawBlock(block) || kindOf(block) != Kind::Divider) return;
    painter.save();
    // Прямоугольник блока и так идёт от поля до поля колонки — не во всё окно.
    QPen pen(appearance().dividerColor);
    pen.setWidthF(qMax(1.0, appearance().dividerWidth * zoom));
    pen.setCapStyle(Qt::FlatCap);
    painter.setPen(pen);
    const qreal y = rect.center().y();
    painter.drawLine(QPointF(rect.left(), y), QPointF(rect.right(), y));
    painter.restore();
}

}  // namespace zametti
