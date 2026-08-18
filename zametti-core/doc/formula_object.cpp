#include "formula_object.h"

#include "doc_model.h"
#include "document_builder.h"
#include "formula.h"
#include "math_scan.h"
#include "settings.h"

#include <QFontInfo>
#include <QPaintEngine>
#include <QFontMetrics>
#include <QFontMetricsF>
#include <QPen>
#include <QTextDocument>
#include <QTextLayout>
#include <QTextLine>
#include <QVariant>

#include <cmath>

namespace zametti {

// --- кэш вёрстки ------------------------------------------------------------

void FormulaObjects::syncConditions(qreal textPixelSize, const QColor& colour, qreal dpr) {
    if (qFuzzyCompare(textPixelSize, pixelSize_) && colour == colour_ && qFuzzyCompare(dpr, dpr_))
        return;
    pixelSize_ = textPixelSize;
    colour_ = colour;
    dpr_ = dpr;
    cache_.clear();
}

void FormulaObjects::syncTextPixelSize(qreal textPixelSize) {
    if (qFuzzyCompare(textPixelSize, pixelSize_)) return;
    pixelSize_ = textPixelSize;
    cache_.clear();
}

const FormulaRender* FormulaObjects::renderFor(const QString& source, const QString& latex,
                                               bool display) {
    if (!Formulas::ready() || pixelSize_ <= 0.0) return nullptr;
    // Род — часть ключа: `$x$` одиночкой в строке и он же внутри строки — две
    // разные вёрстки (кегль и стиль пределов у них разные).
    const QString key = (display ? QStringLiteral("D:") : QStringLiteral("i:")) + source;
    const auto it = cache_.constFind(key);
    if (it != cache_.constEnd()) return &it.value();

    // Кэш переполнился — выбрасываем целиком: считать заново дешевле, чем
    // вести очередь вытеснения ради заметки с тысячей формул.
    if (cache_.size() >= 512) cache_.clear();

    FormulaRender render;
    render.source = source;
    render.latex = latex;
    render.display = display;
    // Стиль вёрстки движку — по ЗАПИСИ: `$$…$$` просит выключной стиль
    // пределов и сумм, даже когда стоит внутри строки; `$…$` — строчный,
    // даже когда liftMath показал его отдельной полосой (кегль полосы при
    // этом display — его задал род объекта выше).
    render.displayStyle = display;
    {
        const std::vector<MathSpan> spans = scanMath(QStringView(source));
        if (spans.size() == 1 && spans.front().start == 0 &&
            spans.front().end == source.size())
            render.displayStyle = spans.front().display;
    }
    // Кегль рода: коэффициент из настроек поверх пиксельного кегля текста.
    render.pixelSize = pixelSize_ * (display ? settings().formulas().displayScale()
                                             : settings().formulas().inlineScale());
    render.colour = colour_;
    render.dpr = dpr_;
    // Предконтроль ДО движка: он молчалив и семь сломанных формул из десяти
    // дорисовывает огрызком без единой жалобы.
    render.error = checkLatex(latex);
    if (render.error.isEmpty()) {
        const FormulaImage drawn =
            Formulas::render(latex, render.displayStyle, render.pixelSize, colour_, dpr_);
        if (drawn.ok()) {
            render.image = drawn.image;
            render.width = drawn.width;
            render.height = drawn.height;
            render.baseline = drawn.baseline;
            render.depth = drawn.depth;
        } else {
            render.error = drawn.error;
        }
    }
    return &*cache_.insert(key, render);
}

// --- выключная: геометрия ----------------------------------------------------

qreal FormulaObjects::boxHeight(const FormulaRender* render, qreal naturalLine) {
    if (render == nullptr) return naturalLine;
    if (!render->error.isEmpty() || render->image.isNull()) return 2.4 * naturalLine;
    return render->height;
}

QSizeF FormulaObjects::bandFor(const FormulaRender* render, qreal columnWidth, qreal naturalLine,
                               qreal gap) {
    // Вёрстки ещё нет (движок не поднят) — держим место по естественной высоте
    // строки: пустоты вместо формулы быть не должно, а как только вёрстка
    // появится, полоса перемерится.
    return QSizeF(columnWidth, boxHeight(render, naturalLine) + gap);
}

QRectF FormulaObjects::rectFor(const QTextBlock& block, const FormulaRender& render, qreal columnWidth) {
    if (render.image.isNull()) return {};
    const QTextLayout* layout = block.isValid() ? block.layout() : nullptr;
    if (layout == nullptr || layout->lineCount() == 0) return {};
    // ГЕОМЕТРИЯ ОДНА С ОТРИСОВКОЙ: полоса во всю ширину колонки, вёрстка по
    // центру внутри неё. Второй копии этого расчёта быть не должно — разойдётся,
    // и уголки окажутся не там, где формула.
    const qreal shift = columnWidth > render.width ? (columnWidth - render.width) / 2.0 : 0.0;
    const QTextLine line = layout->lineAt(0);
    return QRectF(layout->position() + QPointF(line.x() + shift, line.y()),
                  QSizeF(render.width, render.height));
}

// --- отрисовка: вектор на векторном устройстве --------------------------------

namespace {

// Векторное ли устройство под painter'ом. На бумаге (QPdfWriter) и её родне
// формулу рисует сам движок, кривыми и вшитыми шрифтами полного разрешения, —
// экранный растр туда не едет. Решается по устройству, а не флагом: флаг
// однажды забыли бы снять или поставить.
bool vectorDevice(const QPainter& painter) {
    const QPaintEngine* engine = painter.paintEngine();
    if (engine == nullptr) return false;
    switch (engine->type()) {
        case QPaintEngine::Pdf:
        case QPaintEngine::Picture:
        case QPaintEngine::SVG:
            return true;
        default:
            return false;
    }
}

// Нарисовать вёрстку вектором в её прямоугольник. ГЕОМЕТРИЯ ОДНА С КЭШЕМ:
// движку тот же физический кегль (pixelSize × dpr), поверх — уменьшение на
// плотность; парсить логическим кеглем нельзя — вёрстка движка линейна по
// кеглю лишь с точностью до округления метрик глифов, и на плотном экране
// вектор разошёлся бы с зарезервированным местом на доли пикселя. Ложь — не
// нарисовалось, пусть вызывающий рисует растр.
bool paintVector(QPainter& painter, const QPointF& at, const FormulaRender& render) {
    const qreal dpr = render.dpr > 0.0 ? render.dpr : 1.0;
    painter.save();
    painter.translate(at);
    painter.scale(1.0 / dpr, 1.0 / dpr);
    const QString error = Formulas::paintInto(painter, QPointF(0, 0), render.latex,
                                              render.displayStyle, render.pixelSize * dpr,
                                              render.colour);
    painter.restore();
    return error.isEmpty();
}

}  // namespace

// --- выключная: отрисовка ----------------------------------------------------

void FormulaObjects::paint(QPainter& painter, const QRectF& box, const QRectF& frame,
                           const FormulaRender* render, const QString& source,
                           const FormulaPaint& how) {
    painter.save();
    if (render == nullptr || !render->error.isEmpty() || render->image.isNull()) {
        // Битая формула — рамка с исходником, родня рамки «файл не найден»:
        // молчаливый огрызок хуже честной ошибки.
        painter.fillRect(frame, how.page);
        QPen pen(how.rawColour);
        pen.setStyle(Qt::DashLine);
        pen.setWidthF(qMax(1.0, 1.5 * how.scale));
        painter.setPen(pen);
        painter.drawRect(frame.adjusted(0.5, 0.5, -0.5, -0.5));
        painter.setFont(how.baseFont);
        painter.setPen(how.rawColour);
        const QString what = render == nullptr ? QString() : render->error;
        painter.drawText(frame.adjusted(6, 4, -6, -4), Qt::AlignLeft | Qt::TextWordWrap,
                         what.isEmpty() ? source : source + QLatin1Char('\n') + what);
        painter.restore();
        return;
    }
    // Найденное поиском в исходнике формулы — подсветка под всей вёрсткой:
    // куска исходника на картинке не найти.
    if (how.highlighted) {
        QColor colour = how.highlightColour;
        if (!how.highlightCurrent) colour.setAlpha(110);
        painter.fillRect(box.adjusted(-2, -2, 2, 2), colour);
    }
    // НА БУМАГЕ — ВЕКТОРОМ: кривые и вшитые шрифты полного разрешения вместо
    // экранного растра (просьба владельца). Не вышло — растр, как на экране.
    if (vectorDevice(painter) && paintVector(painter, box.topLeft(), *render)) {
        painter.restore();
        return;
    }
    // ПО ЦЕНТРУ ПОЛОСЫ (решение владельца). Прямоугольник — в логических
    // точках, источник — в физических, и никакой плотности у самой картинки
    // (см. formula.cpp): размер вёрстки не зависит ни от плотности экрана, ни
    // от того, как Qt толкует её пометку. Закрашивать под вёрсткой нечего:
    // исходника в тексте блока нет вовсе — там стоит объект.
    painter.drawImage(box, render->image, QRectF(QPointF(0, 0), QSizeF(render->image.size())));
    painter.restore();
}

// --- строчная: посадка и отрисовка -------------------------------------------

QSizeF FormulaObjects::inlineBandFor(const FormulaRender* render, const QString& source,
                                     const QFont& textFont) {
    // Descent — ЦЕЛЫЙ (QFontMetrics): ровно его Qt вычтет из места при
    // AlignBaseline; дробный сажал бы картинку ниже на дробную часть (замер
    // пробника: +4.00 при descent 4.12).
    const int descent = QFontMetrics(textFont).descent();
    if (render != nullptr && render->error.isEmpty() && !render->image.isNull())
        return QSizeF(render->width, render->baseline + descent);
    // Битая формула или движок не поднят: место под исходник текстом в рамке —
    // сидит в строке как текст.
    const QFontMetricsF metrics(textFont);
    return QSizeF(metrics.horizontalAdvance(source) + 8.0, metrics.ascent() + descent);
}

void FormulaObjects::paintInline(QPainter& painter, const QRectF& rect,
                                 const FormulaRender* render, const QString& source,
                                 const QFont& textFont, const FormulaPaint& how) {
    painter.save();
    if (render == nullptr || !render->error.isEmpty() || render->image.isNull()) {
        // Исходник текстом в пунктирной рамке: битую формулу видно, а не
        // потеряно. Фона нет — строка уже закрашена страницей.
        QPen pen(how.rawColour);
        pen.setStyle(Qt::DashLine);
        pen.setWidthF(1.0);
        painter.setPen(pen);
        painter.drawRect(rect.adjusted(0.5, 0.5, -0.5, -0.5));
        painter.setFont(textFont);
        painter.drawText(QPointF(rect.left() + 4.0,
                                 rect.top() + QFontMetricsF(textFont).ascent()),
                         source);
        painter.restore();
        return;
    }
    if (how.highlighted) {
        QColor colour = how.highlightColour;
        if (!how.highlightCurrent) colour.setAlpha(110);
        painter.fillRect(rect.adjusted(-1, -1, 1, 1), colour);
    }
    // НА БУМАГЕ — ВЕКТОРОМ, как у выключной; прищёлкивать к пикселям там
    // нечего и не к чему.
    if (vectorDevice(painter) && paintVector(painter, rect.topLeft(), *render)) {
        painter.restore();
        return;
    }
    // ВЕРХ КАРТИНКИ — НА ФИЗИЧЕСКИЙ ПИКСЕЛЬ. Дробный верх размазывается
    // сглаживанием на ряд ниже, и вёрстка оказывалась на пиксель ниже буквы
    // (замер пробника). Буквы шрифтовый растеризатор прищёлкивает сам —
    // прищёлкиваем и вёрстку.
    const qreal dpr = render->dpr > 0.0 ? render->dpr : 1.0;
    const qreal top = std::round(rect.top() * dpr) / dpr;
    painter.drawImage(QRectF(rect.left(), top, render->width, render->height), render->image,
                      QRectF(QPointF(0, 0), QSizeF(render->image.size())));
    painter.restore();
}

// --- кэш у документа ----------------------------------------------------------

namespace {
const char* const kFormulaCacheProperty = "zametti.formulaCache";

// Исходник строчной формулы → тело для движка. Доллары снимает канон, а не
// счёт знаков: `\$` и прочие края уже решены одним местом (math_scan.h).
QString inlineLatexOf(const QString& source) {
    const std::vector<MathSpan> spans = scanMath(QStringView(source));
    if (spans.size() != 1 || spans.front().start != 0 || spans.front().end != source.size())
        return {};
    return spans.front().body(QStringView(source)).toString();
}

}  // namespace

void attachFormulaCache(QTextDocument& doc, std::shared_ptr<FormulaObjects> cache) {
    if (cache == nullptr) doc.setProperty(kFormulaCacheProperty, QVariant());
    else doc.setProperty(kFormulaCacheProperty, QVariant::fromValue(std::move(cache)));
}

FormulaObjects* formulaCacheOf(const QTextDocument& doc) {
    const QVariant held = doc.property(kFormulaCacheProperty);
    if (!held.isValid()) return nullptr;
    return held.value<std::shared_ptr<FormulaObjects>>().get();
}

qreal columnWidthOf(const QTextDocument& doc, const QTextBlock& block) {
    const QTextFrameFormat root = doc.rootFrame()->frameFormat();
    const qreal width = doc.textWidth() - root.leftMargin() - root.rightMargin() -
                        block.blockFormat().leftMargin();
    // Документ, которому ширину ещё не задали, отдаёт −1: это бывает ровно один
    // раз, до первой раскладки, — и полоса перемерится с настоящей шириной.
    return qMax(16.0, width);
}

QRectF inlineFormulaRect(const QTextDocument& doc, int position) {
    const QTextBlock block = doc.findBlock(position);
    if (!block.isValid()) return {};
    QString source;
    bool found = false;
    for (QTextBlock::iterator it = block.begin(); !it.atEnd(); ++it) {
        const QTextFragment fragment = it.fragment();
        if (!fragment.isValid() || position < fragment.position() ||
            position >= fragment.position() + fragment.length())
            continue;
        if (fragment.charFormat().objectType() != InlineFormulaObject) return {};
        source = fragment.charFormat().property(ObjectSourceProperty).toString();
        found = true;
        break;
    }
    if (!found) return {};

    FormulaObjects* cache = formulaCacheOf(doc);
    const QString latex = inlineLatexOf(source);
    const FormulaRender* render =
        (cache != nullptr && !latex.isEmpty()) ? cache->renderFor(source, latex, false)
                                               : nullptr;
    const QSizeF band = FormulaObjects::inlineBandFor(render, source, doc.defaultFont());

    const QTextLayout* layout = block.layout();
    if (layout == nullptr || layout->lineCount() == 0) return {};
    const int rel = position - block.position();
    const QTextLine line = layout->lineForTextPosition(rel);
    if (!line.isValid()) return {};
    // Та же посадка, что у AlignBaseline (замер пробника): низ места — на
    // базовой линии плюс ЦЕЛЫЙ descent шрифта.
    const qreal baseline = layout->position().y() + line.y() + line.ascent();
    const qreal top = baseline - (band.height() - QFontMetrics(doc.defaultFont()).descent());
    const qreal x = layout->position().x() + line.cursorToX(rel);
    return QRectF(x, top, band.width(), band.height());
}

// --- обработчик объекта --------------------------------------------------------

FormulaObjectHandler::FormulaObjectHandler(QTextDocument* doc) : QObject(doc) {}

QSizeF FormulaObjectHandler::intrinsicSize(QTextDocument* doc, int posInDocument,
                                           const QTextFormat& format) {
    if (doc == nullptr) return {};
    FormulaObjects* cache = formulaCacheOf(*doc);
    if (cache == nullptr) return {};
    // Кегль — по шрифту документа, на каждом вызове: масштаб живёт в нём, а
    // перемер объектов Qt делает раньше, чем вид успевает сверить условия.
    cache->syncTextPixelSize(QFontInfo(doc->defaultFont()).pixelSize());
    setDisplay(format.objectType() == FormulaObject);

    if (display_) {
        // Выключная: полоса во всю ширину колонки. Исходник и тело спрашиваются
        // у блока — одним местом с отрисовкой и геометрией (blockFormulaRef).
        const QTextBlock block = doc->findBlock(posInDocument);
        const BlockFormulaRef ref = blockFormulaRef(block);
        if (!ref.valid || !ref.display) return {};
        const FormulaRender* render = cache->renderFor(ref.source, ref.latex, true);
        const qreal naturalLine = QFontMetricsF(doc->defaultFont()).height();
        const qreal scale = displayScaleOf(*doc);
        return FormulaObjects::bandFor(render, columnWidthOf(*doc, block), naturalLine,
                                       objectBandGap(scale));
    }

    // Строчная: атом по размеру вёрстки, посадка на базовую линию.
    const QString source = format.stringProperty(ObjectSourceProperty);
    const QString latex = inlineLatexOf(source);
    const FormulaRender* render =
        latex.isEmpty() ? nullptr : cache->renderFor(source, latex, false);
    return FormulaObjects::inlineBandFor(render, source, doc->defaultFont());
}

void FormulaObjectHandler::drawObject(QPainter* painter, const QRectF& rect, QTextDocument* doc,
                                      int posInDocument, const QTextFormat& format) {
    Q_UNUSED(posInDocument);
    if (painter == nullptr || doc == nullptr) return;
    setDisplay(format.objectType() == FormulaObject);
    // ВЫКЛЮЧНАЯ ЗДЕСЬ НЕ РИСУЕТСЯ, и это не забывчивость: объект только держит
    // место, а вёрстка ложится поверх готовой страницы (вид, paintFormulaMarks).
    // Замер сессии 5: выделение Qt кладётся на объектную полосу после
    // drawObject, и выбранная формула тонула в заливке.
    if (display_) return;

    // Строчную drawObject рисует САМ: пробник показал, что в строке выделение
    // её не закрывает (заливка ложится под знак, как под букву), и картинка с
    // альфой остаётся видна поверх выделения.
    FormulaObjects* cache = formulaCacheOf(*doc);
    if (cache == nullptr) return;
    cache->syncTextPixelSize(QFontInfo(doc->defaultFont()).pixelSize());
    const QString source = format.stringProperty(ObjectSourceProperty);
    const QString latex = inlineLatexOf(source);
    const FormulaRender* render =
        latex.isEmpty() ? nullptr : cache->renderFor(source, latex, false);
    FormulaPaint how;
    how.rawColour = styleOf(*doc).rawColor();
    how.baseFont = doc->defaultFont();
    how.scale = displayScaleOf(*doc);
    FormulaObjects::paintInline(*painter, rect, render, source, doc->defaultFont(), how);
}

void registerFormulaHandlers(QTextDocument& doc) {
    if (doc.documentLayout() == nullptr) return;
    FormulaObjectHandler* handler = doc.findChild<FormulaObjectHandler*>(
        QString(), Qt::FindDirectChildrenOnly);
    if (handler == nullptr) handler = new FormulaObjectHandler(&doc);
    doc.documentLayout()->registerHandler(FormulaObject, handler);
    doc.documentLayout()->registerHandler(InlineFormulaObject, handler);
}

}  // namespace zametti
