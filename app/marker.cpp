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

bool isTask(Kind kind) {
    return kind == Kind::TaskUnchecked || kind == Kind::TaskChecked;
}

bool drawnCheckbox(Kind kind) {
    return isTask(kind) && appearance().checkboxStyle == CheckboxStyle::Drawn;
}

bool drawnBullet(Kind kind) {
    return kind == Kind::Bullet && appearance().bulletStyle == BulletStyle::Drawn;
}

// Знак маркера — для тех начертаний, где он берётся из шрифта. У нарисованных
// (кружок, рамка) знака нет.
QString markerText(Kind kind, int ordinal) {
    switch (kind) {
        case Kind::Bullet:
            return drawnBullet(kind) ? QString() : appearance().bulletGlyph;
        case Kind::Ordered:
            return QString::number(ordinal) + QStringLiteral(".");
        case Kind::TaskUnchecked:
        case Kind::TaskChecked:
            switch (appearance().checkboxStyle) {
                case CheckboxStyle::Glyph:
                    return kind == Kind::TaskChecked ? QStringLiteral("☑")
                                                     : QStringLiteral("☐");
                case CheckboxStyle::Ascii:
                    return kind == Kind::TaskChecked ? QStringLiteral("[x]")
                                                     : QStringLiteral("[ ]");
                case CheckboxStyle::Drawn:
                    return QString();
            }
            return QString();
        default:
            return QString();
    }
}

QFont markerFont(Kind kind, const QFont& base) {
    QFont font = base;
    if (kind == Kind::Bullet && appearance().bulletStyle == BulletStyle::Glyph)
        font.setPointSizeF(base.pointSizeF() * appearance().bulletScale);
    if (isTask(kind) && appearance().checkboxStyle == CheckboxStyle::Glyph) {
        font.setFamilies({QString(appearance().symbolFamily), QString(appearance().fontFamily)});
        font.setPointSizeF(base.pointSizeF() * appearance().checkboxGlyphScale);
    }
    return font;
}

// Зазор от маркера до текста. В ширинах "A", а не в пробелах: у пропорциональных
// гарнитур пробел вдвое уже буквы, и колонка на нём выходила бы вплотную.
qreal gapFor(Kind kind, const QFont& base) {
    const qreal unit = QFontMetricsF(base).horizontalAdvance(QLatin1Char('A'));
    if (isTask(kind)) return appearance().checkboxTextGap * unit;
    if (kind == Kind::Ordered) return appearance().orderedTextGap * unit;
    return appearance().bulletTextGap * unit;
}

// Сторона рамки чекбокса: ровно высота чернил строчных с выносными элементами,
// от хвоста "y" до верхушки "i". Абстрактные ascent/descent для этого не годятся,
// они описывают кегельную площадку с запасом.
qreal checkboxSide(const QFont& base) {
    return QFontMetricsF(base).tightBoundingRect(QStringLiteral("iy")).height();
}

qreal glyphWidth(Kind kind, int ordinal, const QFont& base) {
    if (drawnCheckbox(kind)) return checkboxSide(base);
    if (drawnBullet(kind)) return QFontMetricsF(base).xHeight() * appearance().bulletDiameter;
    return QFontMetricsF(markerFont(kind, base)).horizontalAdvance(markerText(kind, ordinal));
}

// Опора маркера в координатах документа: правый край его колонки и базовая
// линия первой строки блока. Маркер прижимается к правому краю — тогда у
// нумерованного списка точки стоят одна под другой независимо от числа цифр.
struct Anchor {
    qreal right = 0;
    qreal baseline = 0;
    bool valid = false;
};

Anchor anchorOf(const QTextBlock& block, Kind kind, const QFont& base) {
    const QTextLayout* layout = block.layout();
    if (layout == nullptr || layout->lineCount() == 0) return {};
    const QTextLine line = layout->lineAt(0);
    const QPointF origin = layout->position();
    // Позиция раскладки — левый край содержимого фрейма; левое поле блока в неё
    // не входит, его надо прибавить.
    const qreal textLeft = origin.x() + block.blockFormat().leftMargin();
    return {textLeft - gapFor(kind, base), origin.y() + line.y() + line.ascent(), true};
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

qreal markerColumn(Kind kind, int ordinal, const QFont& base) {
    return glyphWidth(kind, ordinal, base) + gapFor(kind, base);
}

QRectF checkboxRect(const QTextBlock& block, const QFont& base) {
    const Kind kind = kindOf(block);
    if (!isListBlock(block) || !drawnCheckbox(kind)) return {};
    const Anchor anchor = anchorOf(block, kind, base);
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
    const Kind kind = kindOf(block);
    const Anchor anchor = anchorOf(block, kind, base);
    if (!anchor.valid) return;

    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);

    if (drawnCheckbox(kind)) {
        paintCheckbox(painter, checkboxRect(block, base), kind == Kind::TaskChecked);
    } else if (drawnBullet(kind)) {
        const qreal xHeight = QFontMetricsF(base).xHeight();
        const qreal diameter = xHeight * appearance().bulletDiameter;
        // Кружок стоит на средней линии строчных: она у любой гарнитуры именно
        // там, где глаз ждёт буллет.
        const QPointF center(anchor.right - diameter / 2,
                             anchor.baseline - xHeight / 2 -
                                 appearance().bulletRise * xHeight);
        painter.setPen(Qt::NoPen);
        painter.setBrush(appearance().bulletColor);
        painter.drawEllipse(center, diameter / 2, diameter / 2);
    } else {
        const QFont font = markerFont(kind, base);
        const QString text = markerText(kind, ordinalOf(block));
        const QColor color = isTask(kind) ? (kind == Kind::TaskChecked
                                                 ? appearance().checkboxCheckedColor
                                                 : appearance().checkboxUncheckedColor)
                             : kind == Kind::Ordered ? appearance().orderedColor
                                                     : appearance().bulletColor;
        // Поправка по вертикали — от высоты строчных основного шрифта, а не
        // маркерного: маркер должен двигаться относительно текста строки.
        qreal rise = 0;
        if (kind == Kind::Ordered) rise = appearance().orderedRise;
        else if (kind == Kind::Bullet) rise = appearance().bulletRise;
        painter.setFont(font);
        painter.setPen(color);
        painter.drawText(QPointF(anchor.right - QFontMetricsF(font).horizontalAdvance(text),
                                 anchor.baseline - rise * QFontMetricsF(base).xHeight()),
                         text);
    }

    painter.restore();
}

}  // namespace zametti
