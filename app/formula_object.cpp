#include "formula_object.h"

#include "formula.h"
#include "note_view.h"

#include <QFontMetricsF>
#include <QPen>
#include <QTextDocument>
#include <QTextLayout>
#include <QTextLine>

namespace zametti {

// --- кэш вёрстки ------------------------------------------------------------

void FormulaObjects::syncConditions(qreal pixelSize, const QColor& colour, qreal dpr) {
    if (qFuzzyCompare(pixelSize, pixelSize_) && colour == colour_ && qFuzzyCompare(dpr, dpr_))
        return;
    pixelSize_ = pixelSize;
    colour_ = colour;
    dpr_ = dpr;
    cache_.clear();
}

const FormulaRender* FormulaObjects::renderFor(const QString& source, const QString& latex) {
    if (!Formulas::ready() || pixelSize_ <= 0.0) return nullptr;
    const auto it = cache_.constFind(source);
    if (it != cache_.constEnd()) return &it.value();

    // Кэш переполнился — выбрасываем целиком: считать заново дешевле, чем
    // вести очередь вытеснения ради заметки с тысячей формул.
    if (cache_.size() >= 512) cache_.clear();

    FormulaRender render;
    render.source = source;
    render.pixelSize = pixelSize_;
    render.colour = colour_;
    render.dpr = dpr_;
    // Предконтроль ДО движка: он молчалив и семь сломанных формул из десяти
    // дорисовывает огрызком без единой жалобы.
    render.error = checkLatex(latex);
    if (render.error.isEmpty()) {
        const FormulaImage drawn = Formulas::render(latex, true, pixelSize_, colour_, dpr_);
        if (drawn.ok()) {
            render.image = drawn.image;
            render.width = drawn.width;
            render.height = drawn.height;
        } else {
            render.error = drawn.error;
        }
    }
    return &*cache_.insert(source, render);
}

// --- геометрия --------------------------------------------------------------

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

// --- отрисовка --------------------------------------------------------------

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
    // ПО ЦЕНТРУ ПОЛОСЫ (решение владельца). Прямоугольник — в логических
    // точках, источник — в физических, и никакой плотности у самой картинки
    // (см. formula.cpp): размер вёрстки не зависит ни от плотности экрана, ни
    // от того, как Qt толкует её пометку. Закрашивать под вёрсткой нечего:
    // исходника в тексте блока нет вовсе — там стоит объект.
    painter.drawImage(box, render->image, QRectF(QPointF(0, 0), QSizeF(render->image.size())));
    painter.restore();
}

// --- обработчик объекта -----------------------------------------------------

FormulaObjectHandler::FormulaObjectHandler(NoteView* view) : QObject(view), view_(view) {}

QSizeF FormulaObjectHandler::intrinsicSize(QTextDocument* doc, int posInDocument,
                                           const QTextFormat& format) {
    Q_UNUSED(format);
    if (view_ == nullptr || doc == nullptr) return {};
    return view_->formulaBandFor(doc->findBlock(posInDocument));
}

void FormulaObjectHandler::drawObject(QPainter* painter, const QRectF& rect, QTextDocument* doc,
                                      int posInDocument, const QTextFormat& format) {
    Q_UNUSED(painter);
    Q_UNUSED(rect);
    Q_UNUSED(doc);
    Q_UNUSED(posInDocument);
    Q_UNUSED(format);
    // ЗДЕСЬ НЕ РИСУЕТСЯ НИЧЕГО, и это не забывчивость: объект только ДЕРЖИТ
    // МЕСТО, а сама вёрстка ложится поверх готовой страницы
    // (NoteView::paintFormulaMarks). Замер тот же, что у фотографии: выделение
    // Qt кладёт на объект своим проходом, уже после drawObject, и выбранная
    // формула тонула в заливке.
}

}  // namespace zametti
