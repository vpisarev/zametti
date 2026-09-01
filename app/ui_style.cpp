#include "ui_style.h"

#include "zoom_scale.h"

#include <QFontMetricsF>

#include <algorithm>
#include <cmath>

namespace zametti {
namespace {

// СКРЫТЫЕ ОТНОШЕНИЯ. Наружу они не выносятся (решение владельца: «заложить
// внутрь возможность отрегулировать размер иконок и кегль полосы сведений
// относительно кегля панелей — наружу это не выносить»). Каждое число ниже —
// не выдумка, а то отношение, в котором стояли прежние отдельные настройки,
// когда владелец подогнал их друг под друга руками.

// Иконка тулбара: две высоты заглавной «A». При 11 pt даёт 21 точку — ровно
// прежний toolbar.iconSize владельца (замер: zametti-bench ui-metrics).
constexpr qreal kIconInCapHeights = 2.0;
// Полоса сведений: 10 pt при панельных 11.
constexpr qreal kStatusScale = 10.0 / 11.0;
// Поле поиска: прежняя прибавка find.fontDelta = +2 к панельным 14.
constexpr qreal kFindScale = 16.0 / 14.0;
// Поле вокруг иконки в кнопке: прежние 6 точек при иконке 24.
constexpr qreal kButtonPaddingOfIcon = 6.0 / 24.0;
// Промежуток смысловых групп: прежние 16 при иконке 24.
constexpr qreal kGroupSpacingOfIcon = 16.0 / 24.0;
// Поле полосы тулбара сверху и снизу: прежние 4 при иконке 24 (были зашиты
// числом прямо в раскладке).
constexpr qreal kToolbarMarginOfIcon = 4.0 / 24.0;
// Поля полосы сведений: прежние 10 и 4 при высоте строки её шрифта ≈17.
constexpr qreal kStatusPaddingOfHeight = 10.0 / 17.0;
constexpr qreal kStatusPaddingTopOfHeight = 4.0 / 17.0;
// Значок папки в дереве: прежний sidebar.folderScale = 1.05 кегля панели.
constexpr qreal kFolderScale = 1.05;
// Высота строки дерева долей от высоты шрифта: прежний
// sidebar.lineHeightFactor.
constexpr qreal kRowHeightFactor = 1.6;

// Ни один размер не имеет права выродиться в ноль: на самом мелком кегле шкалы
// кнопка без поля ещё жива, а кнопка нулевой стороны — уже нет.
int atLeastOne(qreal value) { return std::max(1, int(std::lround(value))); }

}  // namespace

void ZUiStyle::update(const ZSettings& settings, int zoomSteps) {
    // Гарнитура интерфейса; пусто — гарнитура текста, как это было у всех
    // прежних мест по отдельности.
    const QString family = settings.ui().appFamily().isEmpty()
                               ? settings.style().fontFamily()
                               : settings.ui().appFamily();
    const qreal points = settings.ui().appPoint() * zoomScale(zoomSteps);

    const Stamp want{family, points, zoomSteps};
    if (want == stamp_ && icon_ > 0) return;
    stamp_ = want;
    steps_ = zoomSteps;

    app_ = QFont(family);
    app_.setPointSizeF(points);
    status_ = app_;
    status_.setPointSizeF(points * kStatusScale);
    find_ = app_;
    find_.setPointSizeF(points * kFindScale);

    const QFontMetricsF metrics(app_);
    icon_ = atLeastOne(kIconInCapHeights * metrics.capHeight());
    buttonPadding_ = atLeastOne(kButtonPaddingOfIcon * icon_);
    groupSpacing_ = atLeastOne(kGroupSpacingOfIcon * icon_);
    toolbarMargin_ = atLeastOne(kToolbarMarginOfIcon * icon_);
    // Значок папки — ростом со строку шрифта, взятого чуть крупнее: так он
    // виден рядом с именем и не тонет под ним. Считается ровно как прежде — по
    // высоте строки, а не по capHeight: у иконки папки другая задача, чем у
    // иконки тулбара, и число у неё своё.
    QFont folder = app_;
    folder.setPointSizeF(points * kFolderScale);
    folderIcon_ = atLeastOne(QFontMetricsF(folder).height());

    const qreal statusHeight = QFontMetricsF(status_).height();
    statusPadding_ = atLeastOne(kStatusPaddingOfHeight * statusHeight);
    statusPaddingTop_ = atLeastOne(kStatusPaddingTopOfHeight * statusHeight);
}

qreal ZUiStyle::rowHeightFactor() const { return kRowHeightFactor; }

}  // namespace zametti
