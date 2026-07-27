#include "note_view.h"

#include "marker.h"
#include "settings.h"

#include <QAbstractTextDocumentLayout>
#include <QFontMetricsF>
#include <QPainter>
#include <QPaintEvent>
#include <QResizeEvent>
#include <QScrollBar>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextFrame>
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

void NoteView::setZoom(qreal zoom) {
    zoom_ = zoom;
}

void NoteView::applyContentWidth() {
    const qreal side = appearance().sideMargin * zoom_;
    qreal margin = side;

    if (appearance().maxContentWidth > 0.0) {
        const qreal limit = appearance().maxContentWidth *
                            QFontMetricsF(baseFontFor(zoom_)).horizontalAdvance(QLatin1Char('A'));
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
    root->setFrameFormat(format);
}

void NoteView::resizeEvent(QResizeEvent* event) {
    QTextBrowser::resizeEvent(event);
    applyContentWidth();
}

void NoteView::paintEvent(QPaintEvent* event) {
    QTextBrowser::paintEvent(event);

    const QFont base = baseFontFor(zoom_);
    QPainter painter(viewport());
    painter.translate(-horizontalScrollBar()->value(), -verticalScrollBar()->value());

    const QRectF visible(horizontalScrollBar()->value() + event->rect().x(),
                         verticalScrollBar()->value() + event->rect().y(),
                         event->rect().width(), event->rect().height());

    const QAbstractTextDocumentLayout* layout = document()->documentLayout();
    for (QTextBlock block = document()->begin(); block.isValid(); block = block.next()) {
        const QRectF rect = layout->blockBoundingRect(block);
        if (rect.top() > visible.bottom()) break;
        if (rect.bottom() < visible.top()) continue;
        paintMarker(painter, block, base);
    }
}

}  // namespace zametti
