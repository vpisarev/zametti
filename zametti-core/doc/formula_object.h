// ФОРМУЛА — ОБЪЕКТ ТЕКСТА, и весь её показ живёт здесь, в ядре (решение
// владельца: код формул — в минимальном числе файлов, и он не в виджетах:
// заметка обязана уметь превратиться в PDF из командной строки, где UI нет).
//
// Родов два, а класс один:
//
//   * ВЫКЛЮЧНАЯ (`$$…$$` или `$…$` одиночкой в своей строке — liftMath):
//     отдельный абзац, полоса во всю ширину колонки, вёрстка по центру;
//   * СТРОЧНАЯ (`$…$` внутри строки): атом в строке текста, посадка на
//     базовую линию соседних букв.
//
// В документе формула занимает один знак U+FFFC (FormulaObject или
// InlineFormulaObject в doc_model.h), исходник — в свойстве формата, показ —
// вёрстка MicroTeX. Ядро формул: doc/formula.h (движок), format/math_scan.h
// (канон границ); здесь — связь вёрстки с документом:
//
//   * кэш вёрстки ПО СОДЕРЖИМОМУ (FormulaObjects): ключ — исходник и род,
//     условия (кегль, цвет пера, плотность) сверяются при выдаче. Прежде кэш
//     ключевался номером блока и перестраивался по textChanged — а вёрстка Qt
//     перемеряет объект внутри contentsChange, раньше: формулы ложились на
//     текст («наползают, низ пропадает» у владельца). Кэш прикреплён к самому
//     QTextDocument заметки (attachFormulaCache, тем же приёмом, что стиль):
//     он принадлежит ZDocument, а не виду;
//   * ОДИН обработчик объекта на оба рода (FormulaObjectHandler): режим
//     display/inline выставляется из контекста вызова — по objectType формата;
//   * посадка строчной формулы — ЧИСЛА ПРОБНИКА (zametti-bench inline, снимки
//     в .testdata/inline-formula-shots): verticalAlignment = AlignBaseline,
//     при котором Qt ставит низ отведённого места на «базовая линия + ЦЕЛЫЙ
//     descent шрифта» (QFontMetrics, не QFontMetricsF — замерено), значит
//     место высотой (базовая линия вёрстки + целый descent) сажает картинку
//     точно на базовую линию: ошибка 0.00 px на трёх кеглях × двух плотностях.

#ifndef ZAMETTI_FORMULA_OBJECT_H
#define ZAMETTI_FORMULA_OBJECT_H

#include <QColor>
#include <QFont>
#include <QHash>
#include <QImage>
#include <QObject>
#include <QPainter>
#include <QRectF>
#include <QSizeF>
#include <QString>
#include <QTextBlock>
#include <QTextObjectInterface>

#include <memory>

class QTextDocument;

namespace zametti {

// Формула, показанная вёрсткой: картинка и то, из чего она посчитана.
struct FormulaRender {
    QImage image;          // готовая картинка с домноженной альфой
    QString source;        // исходник с долларами — по нему и считали
    QString latex;         // тело без долларов — им зовётся движок (и вектор)
    QString error;         // непусто — формула битая, рисуется рамка
    qreal width = 0.0;     // логические размеры вёрстки
    qreal height = 0.0;
    qreal baseline = 0.0;  // от верха вёрстки до её базовой линии (посадка строчной)
    qreal depth = 0.0;     // сколько вёрстка свисает ниже базовой линии
    qreal padTop = 0.0;    // поле сверху, добавленное под свисающие чернила
    qreal padLeft = 0.0;   // и слева
    bool display = true;   // род ОБЪЕКТА: выключная полосой или строчная в строке
    bool displayStyle = true;  // стиль вёрстки движку — по записи ($$ или $)
    qreal pixelSize = 0.0;
    QColor colour;
    qreal dpr = 1.0;
};

// Что нужно отрисовке формулы от показа. Плоская структура: рисуют и на
// экране, и на бумаге, и в наборах.
struct FormulaPaint {
    QColor page;            // фон под рамкой битой формулы
    QColor rawColour;       // перо рамки и текста ошибки
    QFont baseFont;         // шрифт текста ошибки и мера «естественной строки»
    qreal scale = 1.0;      // displayScale показа
    QColor highlightColour; // подсветка поиска
    bool highlighted = false;
    bool highlightCurrent = false;
};

// Зазор-воздух полосы объекта (фотографии, формулы, таблицы) — одно число на
// всех, иначе объекты дышат по-разному.
inline qreal objectBandGap(qreal scale) { return 6.0 * scale; }

class FormulaObjects {
public:
    // Условия вёрстки: КЕГЛЬ ОКРУЖАЮЩЕГО ТЕКСТА движку В ПИКСЕЛЯХ (пункты дали
    // бы формулу на треть мельче — обжёгся в пробнике), цвет пера палитры (в
    // тёмной теме формула набрана светлым, а не вывернута), плотность экрана.
    // Коэффициенты родов (formulas.displayScale / inlineScale) кэш применяет
    // САМ — вызывающий о них не знает. Разошлись условия с теми, при которых
    // собран кэш, — кэш пуст. Дёшево; зовётся при смене облика, масштаба и на
    // каждой выдаче.
    void syncConditions(qreal textPixelSize, const QColor& colour, qreal dpr);
    // Сверка ОДНОГО кегля — для обработчика объектов: кегль он знает сам, по
    // шрифту документа (масштаб живёт в нём), а цвет и плотность — дело того,
    // кто показывает. Разошёлся кегль — кэш пуст, прочие условия целы.
    void syncTextPixelSize(qreal textPixelSize);
    // Вёрстка по исходнику: из кэша или заново (движок зовётся здесь и только
    // здесь). display — род ОБЪЕКТА (выключная полосой или строчная в строке);
    // сколько долларов у самого исходника, разбирается внутри. nullptr —
    // движок не поднят или условий ещё нет.
    const FormulaRender* renderFor(const QString& source, const QString& latex, bool display);

    // --- выключная: полоса во всю ширину колонки --------------------------

    // Сколько места занимает формула: вёрстка или рамка ошибки. Одним местом —
    // иначе резерв и отрисовка разойдутся, и рамка налезет на текст под собой.
    static qreal boxHeight(const FormulaRender* render, qreal naturalLine);
    // Полоса объекта: вся ширина колонки × (вёрстка + воздух).
    static QSizeF bandFor(const FormulaRender* render, qreal columnWidth, qreal naturalLine,
                          qreal gap);
    // Прямоугольник вёрстки в координатах документа: по центру полосы, под
    // верхом строки. Пустой — блок не размечен или вёрстки нет.
    static QRectF rectFor(const QTextBlock& block, const FormulaRender& render, qreal columnWidth);
    // Нарисовать вёрстку (или рамку битой формулы с исходником) в её
    // прямоугольнике; frame — полоса под рамку ошибки, box — место вёрстки.
    static void paint(QPainter& painter, const QRectF& box, const QRectF& frame,
                      const FormulaRender* render, const QString& source, const FormulaPaint& how);

    // --- строчная: атом в строке текста -----------------------------------

    // Место строчной формулы (intrinsicSize): ширина вёрстки × (базовая линия
    // вёрстки + целый descent шрифта). Вместе с AlignBaseline это сажает
    // картинку точно на базовую линию строки (пробник). Вёрстки нет (битая или
    // движок не поднят) — место под исходник текстом в рамке.
    static QSizeF inlineBandFor(const FormulaRender* render, const QString& source,
                                const QFont& textFont);
    // Нарисовать строчную формулу в отведённом Qt прямоугольнике: картинка
    // верхом на физический пиксель (дробный верх размазывается сглаживанием на
    // ряд ниже — замер пробника), битая — исходник текстом в пунктирной рамке.
    static void paintInline(QPainter& painter, const QRectF& rect, const FormulaRender* render,
                            const QString& source, const QFont& textFont, const FormulaPaint& how);

private:
    QHash<QString, FormulaRender> cache_;
    qreal pixelSize_ = 0.0;
    QColor colour_;
    qreal dpr_ = 0.0;
};

// Кэш вёрстки прикреплён к самому QTextDocument заметки — тем же приёмом, что
// стиль (attachStyle/styleOf): владеет им ZDocument, а вид и бумага только
// спрашивают документ. nullptr — к этому документу кэш не прикрепляли
// (чужой временный документ).
void attachFormulaCache(QTextDocument& doc, std::shared_ptr<FormulaObjects> cache);
FormulaObjects* formulaCacheOf(const QTextDocument& doc);

// ОДИН ОБРАБОТЧИК НА ОБА РОДА ФОРМУЛ. Только РАЗМЕР и отрисовка строчной:
// выключная рисуется поверх готовой страницы видом (замер сессии 5: выделение
// Qt кладётся на объектную полосу после drawObject), а строчную drawObject
// рисует сам — пробник показал, что в строке выделение её не закрывает.
class FormulaObjectHandler : public QObject, public QTextObjectInterface {
    Q_OBJECT
    Q_INTERFACES(QTextObjectInterface)

public:
    explicit FormulaObjectHandler(QTextDocument* doc);

    QSizeF intrinsicSize(QTextDocument* doc, int posInDocument,
                         const QTextFormat& format) override;
    void drawObject(QPainter* painter, const QRectF& rect, QTextDocument* doc,
                    int posInDocument, const QTextFormat& format) override;

private:
    // Режим текущего вызова — из контекста (objectType формата). Оба рода
    // идут одним путём: setDisplay, дальше общий размер и общая отрисовка.
    void setDisplay(bool display) { display_ = display; }
    bool display_ = false;
};

// Зарегистрировать обработчик формул у вёрстки документа — на оба рода.
// Зовёт ZDocument для своего документа; вид зовёт для собственного документа
// Qt (в него собирает вывоз на бумагу). Обработчик — ребёнок документа, один
// на документ: повторный вызов переиспользует его.
void registerFormulaHandlers(QTextDocument& doc);

// Ширина колонки, доступная блоку, — от самого документа (textWidth минус поля
// рамки и отступ блока): на бумаге ширина своя, и мерить надо ту, по которой
// Qt раскладывает.
qreal columnWidthOf(const QTextDocument& doc, const QTextBlock& block);

// Прямоугольник строчной формулы по позиции её знака, в координатах документа —
// подсветке поиска и наборам. Те же числа, что у intrinsicSize (посадка одна с
// вёрсткой Qt). Пустой — там не строчная формула или строка ещё не сверстана.
QRectF inlineFormulaRect(const QTextDocument& doc, int position);

}  // namespace zametti

Q_DECLARE_METATYPE(std::shared_ptr<zametti::FormulaObjects>)

#endif  // ZAMETTI_FORMULA_OBJECT_H
