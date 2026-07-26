// Все крутилки внешнего вида в одном месте.
//
// Пока это константы в коде. Когда дойдём до настроек, отсюда они переедут в
// конфиг целиком, и переезд будет механическим: ни одного числа и ни одного
// цвета за пределами этого файла нет.

#ifndef ZAMETTI_APPEARANCE_H
#define ZAMETTI_APPEARANCE_H

#include <QColor>
#include <QLatin1String>

namespace zametti::appearance {

// --- шрифт ------------------------------------------------------------------

inline constexpr QLatin1String kFontFamily{"IBM Plex Mono"};
inline constexpr qreal kBaseFontPoint = 11.0;

// В IBM Plex Mono нет ни U+2610, ни U+2611. Нужен только шрифтовым вариантам
// чекбокса (см. CheckboxStyle), нарисованный обходится без него.
inline constexpr QLatin1String kSymbolFamily{"DejaVu Sans Mono"};

// Кегль заголовков 1..6 относительно базового.
inline constexpr qreal kHeadingScale[6] = {1.7, 1.45, 1.25, 1.1, 1.0, 0.95};

// Эмодзи приходят из запасного шрифта и рядом с моноширинным текстом смотрятся
// мелко: у них другая нормаль по кеглю. Множитель применяется к любому знаку,
// которого нет в основной гарнитуре.
inline constexpr qreal kFallbackScale = 1.15;

// --- ритм страницы ----------------------------------------------------------

// У IBM Plex Mono собственный межстрочный просвет уже приличный, поэтому
// множитель нужен маленький. Пункты списка ставим плотно: список читается как
// один объект. Расстояние между абзацами держат поля блока, а не интерлиньяж —
// иначе, ужимая строки, мы бы заодно сплющили и абзацы.
inline constexpr qreal kLineHeightFactor = 1.15;
inline constexpr qreal kListLineHeightFactor = 1.05;
inline constexpr qreal kBlockSpacing = 13.0;

// Поля страницы. Боковые заметно больше вертикальных: строка не должна упираться
// в край окна, читать так тяжело.
inline constexpr qreal kSideMargin = 56.0;
inline constexpr qreal kVerticalMargin = 24.0;

// --- цвета ------------------------------------------------------------------

inline const QColor kPageBackground(0xfe, 0xfe, 0xfb);
inline const QColor kSelectionBackground(0xbf, 0xdb, 0xfe);
inline const QColor kLinkColor(0x32, 0x5c, 0xc0);
inline const QColor kMarkerColor(0x7a, 0x82, 0x8c);   // "•" и "1." у списков
inline const QColor kQuoteColor(0x5a, 0x62, 0x6a);
inline const QColor kRawColor(0x99, 0x9f, 0xa6);      // непонятое, дословный кусок
inline const QColor kCodeBackground(0, 0, 0, 14);

// --- чекбокс ----------------------------------------------------------------

// Отмеченный: заливка этим цветом и галочка поверх неё.
inline const QColor kCheckboxCheckedColor(0x32, 0x5c, 0xc0);
// Пустой: только рамка, без заливки.
inline const QColor kCheckboxUncheckedColor(0x32, 0x5c, 0xc0);
inline const QColor kCheckboxTickColor(0xff, 0xff, 0xff);

inline constexpr qreal kCheckboxPenWidth = 1.4;
inline constexpr qreal kCheckboxCornerRadius = 2.5;

// Оптическая поправка положения. Геометрически рамка совпадает с чернилами букв
// (от хвоста "y" до верхушки "i"), но читается чуть низкой: у сплошного
// прямоугольника масса распределена равномерно, а у строчных букв собрана выше.
// Доля от высоты, а не пиксели: должна пережить смену кегля.
inline constexpr qreal kCheckboxOpticalRise = 0.03;

// Кегль шрифтовых вариантов чекбокса. К нарисованному отношения не имеет.
inline constexpr qreal kCheckboxGlyphScale = 1.8;

}  // namespace zametti::appearance

#endif  // ZAMETTI_APPEARANCE_H
