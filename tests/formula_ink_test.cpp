// РАСТР ФОРМУЛЫ ДЕРЖИТ ВСЕ ЕЁ ЧЕРНИЛА (жалоба владельца: «на некоторых
// уровнях зума явно режутся нижние части формул», $e^{-x}$ и $x_0^2$ в
// «Typesetting Math in Markdown»).
//
// Коробка движка — величина ЛОГИЧЕСКАЯ: getHeight() описывает вёрстку, а не
// пиксели, и глифы рисуются за неё — хвост «γ», круглый низ «e», скобки, знак
// корня, нижний индекс. Растр ровно по коробке эти хвосты срезал; замер: из
// 190 вёрсток на кеглях 12…30 за коробку выходили 172, на пиксель-другой, и
// потому срез то виден, то нет — отсюда «на некоторых масштабах».
//
// Судья здесь ЧУЖОЙ по отношению к растру: та же формула рисуется вторым
// путём — вектором (Formulas::paintInto) на холст с большими полями, — и
// границы её чернил сверяются с коробкой, которую объявил растр. Разойдутся —
// значит картинка что-то потеряла.

#include "formula.h"

#include "test_util.h"

#include <QColor>
#include <QImage>
#include <QPainter>
#include <QRect>

#include <cmath>
#include <string>
#include <vector>

namespace {

// Формулы с хвостами во все стороны: под базовую линию, над коробкой, за края.
const std::vector<QString> kSamples = {
    QStringLiteral("e^{-x}"),   QStringLiteral("x_0^2"),      QStringLiteral("\\gamma)"),
    QStringLiteral("x_j"),      QStringLiteral("\\frac{a}{b}"), QStringLiteral("\\sum_{k}"),
    QStringLiteral("(x+y)"),    QStringLiteral("\\log(1-x)"), QStringLiteral("\\sqrt{2}"),
    QStringLiteral("a_n=0"),    QStringLiteral("y_{i}^{2}"),  QStringLiteral("A \\Rightarrow B"),
};

// Границы чернил на холсте; пусто — не нарисовалось ничего.
QRect inkBounds(const QImage& canvas) {
    int top = -1, bottom = -1, left = -1, right = -1;
    for (int y = 0; y < canvas.height(); ++y) {
        const auto* row = reinterpret_cast<const QRgb*>(canvas.constScanLine(y));
        for (int x = 0; x < canvas.width(); ++x) {
            if (qAlpha(row[x]) == 0) continue;
            if (top < 0) top = y;
            bottom = y;
            if (left < 0 || x < left) left = x;
            if (x > right) right = x;
        }
    }
    if (top < 0) return {};
    return QRect(QPoint(left, top), QPoint(right, bottom));
}

void checkRasterHoldsAllInk() {
    int checked = 0;
    for (qreal points = 12.0; points <= 30.0; points += 1.0) {
        for (const QString& latex : kSamples) {
            const zametti::FormulaImage got =
                zametti::Formulas::render(latex, false, points, Qt::black, 1.0);
            if (!got.ok()) continue;

            // ВТОРОЙ ПУТЬ: тот же исходник вектором на холст с полями. Поля
            // заведомо больше любого хвоста, поэтому здесь ничего не срежется
            // и границы чернил настоящие.
            const int pad = 40;
            QImage canvas(int(got.width) + 2 * pad, int(got.height) + 2 * pad,
                          QImage::Format_ARGB32_Premultiplied);
            canvas.fill(Qt::transparent);
            {
                QPainter painter(&canvas);
                painter.setRenderHint(QPainter::Antialiasing, true);
                const QString error = zametti::Formulas::paintInto(
                    painter, QPointF(pad, pad), latex, false, points, Qt::black);
                if (!error.isEmpty()) continue;
            }
            const QRect ink = inkBounds(canvas);
            if (ink.isNull()) continue;
            ++checked;

            // Коробка растра в тех же координатах: её левый верх сдвинут от
            // коробки движка на добавленные поля.
            const QRect box(pad - int(got.padLeft), pad - int(got.padTop), int(got.width),
                            int(got.height));
            const std::string who =
                latex.toStdString() + " на кегле " + std::to_string(int(points));
            ZT_TRUE(who + ": низ чернил внутри картинки (" + std::to_string(ink.bottom()) +
                          " <= " + std::to_string(box.bottom()) + ")", ink.bottom() <= box.bottom());
            ZT_TRUE(who + ": верх чернил внутри картинки", ink.top() >= box.top());
            ZT_TRUE(who + ": левый край чернил внутри картинки", ink.left() >= box.left());
            ZT_TRUE(who + ": правый край чернил внутри картинки", ink.right() <= box.right());
        }
    }
    ZT_TRUE("вёрсток проверено: " + std::to_string(checked), checked >= 100);
}

// Геометрия остаётся связной: базовая линия и глубина описывают ТУ ЖЕ
// картинку, что отдана наружу. Иначе расширение коробки увело бы посадку
// строчной формулы на базовую линию строки.
void checkGeometryStaysConsistent() {
    for (qreal points = 12.0; points <= 24.0; points += 2.0) {
        for (const QString& latex : kSamples) {
            const zametti::FormulaImage got =
                zametti::Formulas::render(latex, false, points, Qt::black, 1.0);
            if (!got.ok()) continue;
            const std::string who =
                latex.toStdString() + " на кегле " + std::to_string(int(points));
            ZT_TRUE(who + ": базовая линия внутри картинки",
                    got.baseline >= 0.0 && got.baseline <= got.height);
            ZT_TRUE(who + ": базовая линия плюс глубина — это высота картинки", std::abs(got.baseline + got.depth - got.height) <= 1.01);
            ZT_TRUE(who + ": высота картинки та, что объявлена", std::abs(got.image.height() - got.height) <= 1.01);
            ZT_TRUE(who + ": ширина картинки та, что объявлена", std::abs(got.image.width() - got.width) <= 1.01);
        }
    }
}

}  // namespace

TEST(FormulaInk, All) {
    QString error;
    if (!zametti::Formulas::init(&error)) {
        ZT_TRUE("движок формул поднялся: " + error.toStdString(), false);
        zt::report("растр формулы: чернила");
        return;
    }
    checkRasterHoldsAllInk();
    checkGeometryStaysConsistent();
    zt::report("растр формулы: чернила");
}
