// ZUiStyle — РАЗМЕРЫ ОБОЛОЧКИ, ВЫВЕДЕННЫЕ ИЗ ОДНОГО КЕГЛЯ.
//
// Тулбар, дерево, список заметок, полоса сведений, панель поиска и диалоги
// раньше брали свои числа порознь: у панелей был свой кегль, у полосы сведений
// свой, у иконки — своё число в пикселях, у полей кнопок — своё. Настроить это
// согласованно было нельзя, и config.json владельца состоял ровно из трёх
// строк, которыми он подгонял их друг под друга (sidebar.fontSize 11,
// statusBar.fontPoints 10, toolbar.iconSize 21). На другой машине — другие три
// строки, и опять руками.
//
// Здесь ровно один вход — ШРИФТ ИНТЕРФЕЙСА (гарнитура и кегль из настроек,
// умноженный на ступень масштаба оболочки), и всё остальное выводится из его
// метрик. Поменялся кегль или нажали Ctrl+Alt+− — согласованно поехало всё.
//
// ИКОНКА — ДВЕ ВЫСОТЫ ЗАГЛАВНОЙ «A» (правило владельца). Из четырёх ответов Qt
// на вопрос «какова высота буквы» взят capHeight, и это ЗАМЕР, а не вкус
// (zametti-bench ui-metrics): чернила tightBoundingRect квантуются хинтингом
// (на 11 pt — 11.00, на 12 pt — 12.00, на 14 pt — 13.00, то есть не растут
// вовсе там, где кегль вырос), а capHeight идёт за кеглем гладко. При 11 pt
// правило даёт 20.94 → 21 точку: ровно то число, которое владелец подобрал
// руками.
//
// МАСШТАБ ОБОЛОЧКИ СЮДА ВХОДИТ, А МАСШТАБ ТЕКСТА — НЕТ. Четыре ступени
// независимы (решение владельца): Ctrl+= не двигает тулбар, Ctrl+Alt+= не
// двигает текст заметки, и ни то ни другое не видно на бумаге.

#ifndef ZAMETTI_UI_STYLE_H
#define ZAMETTI_UI_STYLE_H

#include "settings.h"

#include <QFont>

namespace zametti {

class ZUiStyle {
public:
    // Пересчитать по настройкам и ступени масштаба оболочки. Дешёвая: считает
    // метрики трёх шрифтов. Зовётся при старте, при перечитывании конфига и
    // при Ctrl+Alt+±; сама следит, менялся ли вход (см. stamp_).
    void update(const ZSettings& settings, int zoomSteps);

    // --- шрифты ------------------------------------------------------------
    // Общий шрифт интерфейса: дерево, список заметок, диалоги, меню, подписи.
    const QFont& appFont() const { return app_; }
    // Полоса сведений — мельче: это справка, а не содержание.
    const QFont& statusFont() const { return status_; }
    // Поле поиска — крупнее: в него печатают, а не смотрят на него.
    const QFont& findFont() const { return find_; }

    // --- размеры (в ТОЧКАХ ИНТЕРФЕЙСА, не в пикселях устройства) ------------
    // Сторона иконки тулбара и панели поиска.
    int iconSize() const { return icon_; }
    // Поле вокруг иконки внутри кнопки тулбара.
    int buttonPadding() const { return buttonPadding_; }
    // Промежуток между смысловыми группами кнопок.
    int groupSpacing() const { return groupSpacing_; }
    // Поле полосы тулбара сверху и снизу.
    int toolbarMargin() const { return toolbarMargin_; }
    // Поля полосы сведений: по бокам и сверху/снизу.
    int statusPadding() const { return statusPadding_; }
    int statusPaddingTop() const { return statusPaddingTop_; }
    // Значок папки в дереве: чуть крупнее строчной высоты, чтобы не тонуть.
    int folderIconSize() const { return folderIcon_; }
    // Высота строки дерева — долей от высоты шрифта.
    qreal rowHeightFactor() const;

    // Ступень масштаба оболочки, по которой посчитано нынешнее.
    int zoomSteps() const { return steps_; }

protected:
    // Вход, по которому всё посчитано: гарнитура, кегль, ступень. Совпал —
    // считать заново нечего.
    struct Stamp {
        QString family;
        qreal points = 0.0;
        int steps = 0;
        bool operator==(const Stamp& other) const = default;
    };

    Stamp stamp_;
    QFont app_;
    QFont status_;
    QFont find_;
    int steps_ = 0;
    int icon_ = 0;
    int buttonPadding_ = 0;
    int groupSpacing_ = 0;
    int toolbarMargin_ = 0;
    int statusPadding_ = 0;
    int statusPaddingTop_ = 0;
    int folderIcon_ = 0;
};

}  // namespace zametti

#endif  // ZAMETTI_UI_STYLE_H
