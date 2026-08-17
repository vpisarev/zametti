// ВЫКЛЮЧНАЯ ФОРМУЛА — ОБЪЕКТ ТЕКСТА: всё, что вид умеет с ней, — здесь
// (владелец: «код про формулы — в минимальное количество файлов»).
//
// В документе формула занимает один знак U+FFFC (FormulaObject в doc_model.h),
// исходник — в свойстве формата, показ — вёрстка MicroTeX по центру полосы во
// всю ширину колонки, как у фотографии и таблицы. Ядро формул — doc/formula.h
// (движок), format/math_scan.h (канон границ), blockFormulaRef в doc_model и
// openFormula/closeFormula в editor_ops; здесь — связь вёрстки с видом:
//
//   * обработчик объекта — размер полосы для вёрстки Qt (FormulaObjectHandler);
//   * кэш вёрстки ПО СОДЕРЖИМОМУ (FormulaObjects): ключ — исходник, условия
//     (кегль, цвет пера, плотность) сверяются при выдаче. Прежде кэш ключевался
//     номером блока и перестраивался по textChanged — а вёрстка Qt перемеряет
//     объект внутри contentsChange, раньше: после Ctrl+Z (вернуть удалённую
//     формулу), сворачивания раскрытой, набора под формулой полоса выходила в
//     одну строку, и формула ложилась на текст под ней — «наползают, низ
//     пропадает» у владельца. Теперь размер спрашивают по исходнику самого
//     объекта и считают тут же, если вёрстки нет: номера блоков ни при чём;
//   * геометрия вёрстки в документе, отрисовка (вёрстка или рамка битой
//     формулы), подсветка поиска.

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

namespace zametti {

class NoteView;

// Формула, показанная вёрсткой: картинка и то, из чего она посчитана.
struct FormulaRender {
    QImage image;          // готовая картинка с домноженной альфой
    QString source;        // исходник с долларами — по нему и считали
    QString error;         // непусто — формула битая, рисуется рамка
    qreal width = 0.0;     // логические размеры вёрстки
    qreal height = 0.0;
    qreal pixelSize = 0.0;
    QColor colour;
    qreal dpr = 1.0;
};

// Что нужно отрисовке формулы от вида. Плоская структура: рисуют и на экране,
// и на бумаге, и в наборах.
struct FormulaPaint {
    QColor page;            // фон под рамкой битой формулы
    QColor rawColour;       // перо рамки и текста ошибки
    QFont baseFont;         // шрифт текста ошибки и мера «естественной строки»
    qreal scale = 1.0;      // displayScale вида
    QColor highlightColour; // подсветка поиска
    bool highlighted = false;
    bool highlightCurrent = false;
};

class FormulaObjects {
public:
    // Условия вёрстки: кегль движку В ПИКСЕЛЯХ (пункты дали бы формулу на треть
    // мельче — обжёгся в пробнике), цвет пера палитры (в тёмной теме формула
    // набрана светлым, а не вывернута), плотность экрана. Разошлись с теми,
    // при которых собран кэш, — кэш пуст. Дёшево; зовётся при смене облика,
    // масштаба и на каждой выдаче.
    void syncConditions(qreal pixelSize, const QColor& colour, qreal dpr);
    // Вёрстка по исходнику: из кэша или заново (движок зовётся здесь и только
    // здесь). nullptr — движок не поднят или условий ещё нет.
    const FormulaRender* renderFor(const QString& source, const QString& latex);
    void clear() { cache_.clear(); }

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

private:
    QHash<QString, FormulaRender> cache_;
    qreal pixelSize_ = 0.0;
    QColor colour_;
    qreal dpr_ = 0.0;
};

// Обработчик объекта: только РАЗМЕР. Рисует поверх готовой страницы вид (как
// у фото и таблицы: выделение Qt кладётся на объект после drawObject).
class FormulaObjectHandler : public QObject, public QTextObjectInterface {
    Q_OBJECT
    Q_INTERFACES(QTextObjectInterface)

public:
    explicit FormulaObjectHandler(NoteView* view);

    QSizeF intrinsicSize(QTextDocument* doc, int posInDocument,
                         const QTextFormat& format) override;
    void drawObject(QPainter* painter, const QRectF& rect, QTextDocument* doc,
                    int posInDocument, const QTextFormat& format) override;

private:
    NoteView* view_ = nullptr;
};

}  // namespace zametti

#endif  // ZAMETTI_FORMULA_OBJECT_H
