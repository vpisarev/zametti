#include "note_view.h"

#include "doc_model.h"
#include "marker.h"
#include "settings.h"

#include <QAbstractTextDocumentLayout>
#include <QFontMetricsF>
#include <QPainter>
#include <QPaintEvent>
#include <QResizeEvent>
#include <QPalette>
#include <QScrollBar>
#include <QWidget>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextFrame>
#include <QTextLayout>
#include <QTextFrameFormat>

#include <cmath>

namespace zametti {
namespace {

QFont baseFontFor(qreal zoom) {
    QFont font{QString(appearance().fontFamily)};
    font.setPointSizeF(appearance().baseFontPoint * zoom);
    font.setStyleHint(QFont::Monospace);
    return font;
}

}  // namespace

void applyPalette(QWidget& view) {
    QPalette palette = view.palette();
    palette.setColor(QPalette::Base, appearance().pageBackground);
    palette.setColor(QPalette::Highlight, appearance().selectionBackground);
    // Выделение светлое, поэтому текст в нём остаётся тёмным: белый по
    // умолчанию на таком фоне просто пропал бы.
    palette.setColor(QPalette::HighlightedText, palette.color(QPalette::Text));
    view.setPalette(palette);
}

QFont NoteView::baseFont() const { return baseFontFor(zoom_); }

void NoteView::setZoom(qreal zoom) {
    zoom_ = zoom;
}

void NoteView::applyContentWidth() {
    // Поля и предел ширины заданы в ширинах "A" — той же мерой, что и в
    // сборщике документа, иначе при смене гарнитуры они разъехались бы.
    const qreal charUnit =
        QFontMetricsF(baseFontFor(zoom_)).horizontalAdvance(QLatin1Char('A'));
    const qreal side = appearance().sideMargin * charUnit;
    qreal margin = side;

    if (appearance().maxContentWidth > 0.0) {
        const qreal limit = appearance().maxContentWidth * charUnit;
        const qreal extra = (viewport()->width() - 2 * side - limit) / 2;
        if (extra > 0.0) margin = side + extra;
    }

    QTextFrame* root = document()->rootFrame();
    QTextFrameFormat format = root->frameFormat();
    // Сравнение с допуском, а не на равенство: иначе каждый вызов переразмечал
    // бы документ заново.
    if (std::fabs(format.leftMargin() - margin) < 0.01) return;
    format.setLeftMargin(margin);
    format.setRightMargin(margin);
    changingLayout_ = true;
    root->setFrameFormat(format);
    changingLayout_ = false;
}

void NoteView::resizeEvent(QResizeEvent* event) {
    QTextBrowser::resizeEvent(event);
    applyContentWidth();
}

void NoteView::paintCodeBackground(QPainter& painter, const QRectF& visible) {
    const QAbstractTextDocumentLayout* layout = document()->documentLayout();
    const int firstVisible = layout->hitTest(QPointF(0, visible.top()), Qt::FuzzyHit);

    painter.setPen(Qt::NoPen);
    painter.setBrush(appearance().codeBackground);
    for (QTextBlock block = document()->findBlock(firstVisible); block.isValid();
         block = block.next()) {
        const QRectF rect = layout->blockBoundingRect(block);
        if (rect.top() > visible.bottom()) break;
        if (rect.bottom() < visible.top()) continue;
        if (isRawBlock(block) || kindOf(block) != Kind::Code) continue;

        // Высоту считаем по числу строк и назначенной высоте строки, а не по
        // прямоугольнику блока: прямоугольник отдаёт естественную высоту, а
        // шаг идёт по назначенной, и разница между ними — та самая полоса.
        // Строк в блоке может быть больше одной: длинная строка кода переносится.
        const qreal assigned = block.blockFormat().lineHeight();
        const int lines = block.layout() != nullptr ? block.layout()->lineCount() : 1;
        const qreal height =
            assigned > 0 ? qMax(rect.height(), lines * assigned) : rect.height();
        painter.drawRect(QRectF(rect.left(), rect.top(), rect.width(), height));
    }
}

void NoteView::paintSelectionBackground(QPainter& painter, const QRectF& visible) {
    const QTextCursor cursor = textCursor();
    if (!cursor.hasSelection()) return;

    const int from = cursor.selectionStart();
    const int to = cursor.selectionEnd();
    const QAbstractTextDocumentLayout* layout = document()->documentLayout();

    painter.setPen(Qt::NoPen);
    painter.setBrush(palette().color(QPalette::Highlight));
    for (QTextBlock block = document()->findBlock(from); block.isValid();
         block = block.next()) {
        if (block.position() > to) break;
        const QRectF bounds = layout->blockBoundingRect(block);
        if (bounds.top() > visible.bottom()) break;
        if (bounds.bottom() < visible.top()) continue;

        const QTextLayout* lines = block.layout();
        if (lines == nullptr) continue;
        const qreal assigned = block.blockFormat().lineHeight();

        for (int i = 0; i < lines->lineCount(); ++i) {
            const QTextLine line = lines->lineAt(i);
            const int start = qMax(from - block.position(), line.textStart());
            const int stop = qMin(to - block.position(), line.textStart() + line.textLength());
            if (start >= stop) continue;

            // Закрашиваем только недостающую полоску под строкой — от её
            // естественного низа до назначенного. Саму строку Qt закрасит сам, и
            // подменять его заливку незачем: ширину хвостика за концом строки он
            // считает по своим правилам.
            const qreal gap = assigned - line.height();
            if (gap <= 0.0) continue;

            const qreal x1 = bounds.left() + line.cursorToX(start);
            const qreal x2 = bounds.left() + line.cursorToX(stop);
            const qreal top = bounds.top() + line.y() + line.height();
            painter.drawRect(QRectF(x1, top, x2 - x1, gap));
        }
    }
}

void NoteView::paintEvent(QPaintEvent* event) {
    {
        // Рисуем до текста: сам виджет виден только там, где Qt уже стёр фон, а
        // текст ляжет поверх нашей заливки.
        QPainter painter(viewport());
        painter.translate(-horizontalScrollBar()->value(), -verticalScrollBar()->value());
        const QRectF visible(horizontalScrollBar()->value() + event->rect().x(),
                             verticalScrollBar()->value() + event->rect().y(),
                             event->rect().width(), event->rect().height());
        paintCodeBackground(painter, visible);
        paintSelectionBackground(painter, visible);
    }
    QTextBrowser::paintEvent(event);

    const QFont base = baseFontFor(zoom_);
    QPainter painter(viewport());
    painter.translate(-horizontalScrollBar()->value(), -verticalScrollBar()->value());

    const QRectF visible(horizontalScrollBar()->value() + event->rect().x(),
                         verticalScrollBar()->value() + event->rect().y(),
                         event->rect().width(), event->rect().height());

    // К первому видимому блоку идём поиском по раскладке, а не обходом от
    // начала документа: обход стоит тем дороже, чем ниже прокрутка, и на
    // заметке в тысячу блоков это уже заметно.
    const QAbstractTextDocumentLayout* layout = document()->documentLayout();
    const int firstVisible = layout->hitTest(QPointF(0, visible.top()), Qt::FuzzyHit);
    for (QTextBlock block = document()->findBlock(firstVisible); block.isValid();
         block = block.next()) {
        const QRectF rect = layout->blockBoundingRect(block);
        if (rect.top() > visible.bottom()) break;
        if (rect.bottom() < visible.top()) continue;
        paintMarker(painter, block, base);
    }
}

}  // namespace zametti
