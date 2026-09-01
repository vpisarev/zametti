// ПРОБНИК МЕТРИК ИНТЕРФЕЙСА: из чего выводить размер иконки.
//
// Вопрос владельца прямой: «возможно нужно размер иконок пересчитывать из
// размера букв, например 2 буквы „A“ по высоте». Проверить это надо ЗАМЕРОМ, а
// не рассуждением: у «высоты буквы» в Qt как минимум четыре разных ответа
// (height, ascent, capHeight, чернила tightBoundingRect), и они расходятся на
// треть. Числа, которые ищем, известны заранее — владелец подобрал их руками в
// своём config.json: кегль панелей 11, кегль полосы сведений 10, иконка 21.
//
//   zametti-bench ui-metrics [кегль ...]

#include "resources.h"
#include "settings.h"
#include "zoom_scale.h"

#include <QFont>
#include <QFontInfo>
#include <QFontMetricsF>
#include <QString>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

void row(const QString& family, double points) {
    QFont font(family);
    font.setPointSizeF(points);
    const QFontMetricsF m(font);
    // Гарнитура обязана быть ТА САМАЯ: если шрифт не нашёлся и Qt подставила
    // свой, все числа ниже — про чужой шрифт, и правило по ним не выведешь.
    const QString got = QFontInfo(font).family();
    const double ink = m.tightBoundingRect(QStringLiteral("A")).height();
    std::printf("  %-28s→%-28s %5.1f pt | height %6.2f | ascent %6.2f | cap %6.2f | "
                "чернила A %6.2f | 2×чернил %5.1f | 2×cap %5.1f → %d\n",
                family.toUtf8().constData(), got.toUtf8().constData(), points, m.height(),
                m.ascent(), m.capHeight(), ink, 2.0 * ink, 2.0 * m.capHeight(),
                int(std::lround(2.0 * m.capHeight())));
}

}  // namespace

int ztUiMetricsProbe(int argc, char** argv) {
    zametti::loadEmbeddedFonts();

    std::vector<double> points;
    for (int i = 1; i < argc; ++i) points.push_back(std::atof(argv[i]));
    if (points.empty()) points = {9.0, 10.0, 11.0, 12.0, 14.0, 16.0, 22.0};

    std::printf("МЕТРИКИ ШРИФТА ИНТЕРФЕЙСА. Ищем правило, дающее иконку 21 точку\n"
                "при кегле 11 — ровно то, что владелец подобрал руками.\n\n");
    for (const QString& family : {QStringLiteral("IBM Plex Sans SemiCondensed"),
                                  QStringLiteral("IBM Plex Sans"),
                                  QStringLiteral("IBM Plex Mono")}) {
        std::printf("%s:\n", family.toUtf8().constData());
        for (double p : points) row(family, p);
        std::printf("\n");
    }

    // Пропорциональность: правило годится, только если иконка растёт РОВНО
    // вместе с кеглем — иначе на ступенях масштаба оболочки она поедет.
    std::printf("ПРОПОРЦИОНАЛЬНОСТЬ (IBM Plex Sans SemiCondensed):\n");
    QFont base(QStringLiteral("IBM Plex Sans SemiCondensed"));
    base.setPointSizeF(11.0);
    const double inkUnit =
        QFontMetricsF(base).tightBoundingRect(QStringLiteral("A")).height();
    const double capUnit = QFontMetricsF(base).capHeight();
    for (int k : {-12, -6, -3, 0, 3, 6, 12}) {
        QFont font = base;
        font.setPointSizeF(11.0 * zametti::zoomScale(k));
        const QFontMetricsF m(font);
        const double ink = m.tightBoundingRect(QStringLiteral("A")).height();
        std::printf("  ступень %+3d (×%.4f): чернила %6.2f (2× = %5.1f, отклонение %+5.2f) | "
                    "cap %6.2f (2× = %5.1f → %2d, отклонение %+5.2f)\n",
                    k, zametti::zoomScale(k), ink, 2.0 * ink,
                    2.0 * ink - 2.0 * inkUnit * zametti::zoomScale(k), m.capHeight(),
                    2.0 * m.capHeight(), int(std::lround(2.0 * m.capHeight())),
                    2.0 * m.capHeight() - 2.0 * capUnit * zametti::zoomScale(k));
    }
    return 0;
}
