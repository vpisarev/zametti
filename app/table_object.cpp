#include "table_object.h"

#include "doc_model.h"
#include "note_view.h"

#include <QTextDocument>
#include <QTextLayout>
#include <QTextLine>

namespace zametti {

// --- кэш раскладок ----------------------------------------------------------

const TableRender* TableObjects::renderFor(const QString& source, const TableSpace& space,
                                           const ZDocStyle& style) {
    const auto it = cache_.constFind(source);
    if (it != cache_.constEnd()) {
        const TableRender& had = it.value();
        if (qFuzzyCompare(had.space.columnWidth, space.columnWidth) &&
            qFuzzyCompare(had.space.fullWidth, space.fullWidth) &&
            qFuzzyCompare(had.space.zoom, space.zoom) && had.style == &style)
            return &had;
    }
    // Переполнился — выбросить целиком: считать заново дешевле, чем вести
    // очередь вытеснения ради заметки с сотней таблиц (раскладка 50×8 — 1.6 мс).
    if (cache_.size() >= 256) cache_.clear();

    TableRender render;
    render.source = source;
    render.space = space;
    render.style = &style;
    render.table = parseTable(source);
    render.layout = layoutTable(render.table, space, style);
    return &*cache_.insert(source, std::move(render));
}

QSizeF TableObjects::bandFor(const TableRender& render, qreal columnWidth, qreal gap) {
    // Битый исходник (разбор не сошёлся) — полоса всё равно не пустая: объект
    // нулевой высоты был бы невидимым блоком с блуждающей кареткой. Рамку с
    // исходником рисует paint.
    const qreal height = render.layout.rows > 0 ? render.layout.height : 3.0 * gap;
    return QSizeF(columnWidth, height + gap);
}

QRectF TableObjects::rectFor(const QTextBlock& block, const TableRender& render, qreal gap) {
    const QTextLayout* layout = block.isValid() ? block.layout() : nullptr;
    if (layout == nullptr || layout->lineCount() == 0) return {};
    const QTextLine line = layout->lineAt(0);
    // Сетка стоит у левого края полосы, чуть ниже её верха — на половине
    // воздуха, чтобы не липнуть к строке над ней.
    const QPointF origin = layout->position() + QPointF(line.x(), line.y() + gap / 2.0);
    const qreal width = render.layout.rows > 0 ? render.layout.width : line.width();
    const qreal height = render.layout.rows > 0 ? render.layout.height : qMax(0.0, line.height() - gap);
    return QRectF(origin, QSizeF(width, height));
}

// --- отрисовка --------------------------------------------------------------

void TableObjects::paint(QPainter& painter, const QRectF& area, const TableRender& render,
                         const TablePaint& how) {
    const TableLayout& table = render.layout;
    const ZSettings::Tables& look = how.look;
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, false);

    if (table.rows <= 0) {
        // Битая таблица — рамка с исходником, как у битой формулы: молчаливый
        // огрызок хуже честной ошибки.
        QPen pen(how.text);
        pen.setStyle(Qt::DashLine);
        pen.setWidthF(qMax(1.0, 1.5 * how.scale));
        painter.setPen(pen);
        painter.drawRect(area.adjusted(0.5, 0.5, -0.5, -0.5));
        painter.drawText(area.adjusted(6, 4, -6, -4), Qt::AlignLeft | Qt::TextWordWrap, render.source);
        painter.restore();
        return;
    }

    // Заливки: тело, зебра, шапка. Прозрачные по умолчанию — тогда просто
    // ничего не рисуется.
    const auto fill = [&painter](const QRectF& rect, const QColor& colour) {
        if (colour.alpha() > 0) painter.fillRect(rect, colour);
    };
    fill(area, look.tableColor());
    qreal y = area.top();
    for (int row = 0; row < table.rows; ++row) {
        const qreal height = table.rowHeight.at(row);
        const QRectF band(area.left(), y, area.width(), height);
        if (row == 0) fill(band, look.headerColor());
        else if ((row % 2) == 0) fill(band, look.altTableColor());
        y += height;
    }

    // Подсветка поиска — под линиями и текстом, поверх заливок.
    for (const TablePaint::Highlight& hit : how.highlights) {
        const QRectF rect = highlightRect(render, area, hit.from, hit.to);
        if (rect.isEmpty()) continue;
        QColor colour = how.highlightColour;
        if (!hit.current) colour.setAlpha(110);
        painter.fillRect(rect, colour);
    }

    // Линии. Толщины и цвет — из конфига; ноль означает «не рисовать».
    const qreal scale = how.scale;
    const auto line = [&painter, &look, scale](const QRectF& rect, qreal width) {
        if (width <= 0.0) return;
        painter.fillRect(QRectF(rect.left(), rect.top(), rect.width(), qMax(1.0, width * scale)),
                         look.borderColor());
    };
    const auto column = [&painter, &look, scale](qreal x, qreal top, qreal height, qreal width) {
        if (width <= 0.0) return;
        painter.fillRect(QRectF(x, top, qMax(1.0, width * scale), height), look.borderColor());
    };

    line(QRectF(area.left(), area.top(), area.width(), 0), look.horizontalBorder());
    line(QRectF(area.left(), area.bottom() - look.horizontalBorder() * scale, area.width(), 0),
         look.horizontalBorder());
    y = area.top();
    for (int row = 0; row < table.rows; ++row) {
        y += table.rowHeight.at(row);
        if (row == 0)
            line(QRectF(area.left(), y - look.headerSeparator() * scale / 2, area.width(), 0),
                 look.headerSeparator());
        else if (row + 1 < table.rows)
            line(QRectF(area.left(), y, area.width(), 0), look.rowSeparator());
    }
    column(area.left(), area.top(), area.height(), look.verticalBorder());
    column(area.right() - look.verticalBorder() * scale, area.top(), area.height(),
           look.verticalBorder());
    qreal x = area.left();
    for (int col = 0; col + 1 < table.columns; ++col) {
        x += table.columnWidth.at(col);
        column(x, area.top(), area.height(), look.columnSeparator());
    }

    // Текст ячеек. Выравнивание — из :---: разбора; по умолчанию влево, как в
    // GitHub.
    const ZDocStyle& style = render.style != nullptr ? *render.style : settings().style();
    const qreal padX = tableCellPadX(table.scale, style);
    const qreal padY = tableCellPadY(table.scale, style);
    painter.setPen(how.text);
    for (const TableCellBox& cell : table.cells) {
        if (cell.text == nullptr) continue;
        const QRectF box = cell.rect.translated(area.topLeft());
        qreal cx = box.left() + padX;
        if (cell.align == TableAlign::Right)
            cx = box.right() - padX - cell.textWidth;
        else if (cell.align == TableAlign::Center)
            cx = box.left() + (box.width() - cell.textWidth) / 2;
        cell.text->draw(&painter, QPointF(cx, box.top() + padY));
    }
    painter.restore();
}

// --- попадание --------------------------------------------------------------

bool TableObjects::cellAt(const TableRender& render, const QRectF& area, const QPointF& point,
                          int* row, int* column, int* sourceOffset) {
    const TableLayout& table = render.layout;
    if (table.rows <= 0 || !area.contains(point)) return false;
    // РЯД по вертикали, КОЛОНКА по горизонтали; за краем последней — последняя.
    int r = 0;
    qreal y = area.top();
    for (; r + 1 < table.rows; ++r) {
        if (point.y() < y + table.rowHeight.at(r)) break;
        y += table.rowHeight.at(r);
    }
    int c = 0;
    qreal x = area.left();
    for (; c + 1 < table.columns; ++c) {
        if (point.x() < x + table.columnWidth.at(c)) break;
        x += table.columnWidth.at(c);
    }
    if (row != nullptr) *row = r;
    if (column != nullptr) *column = c;
    if (sourceOffset != nullptr) {
        const TableCell* cell = render.table.cellAt(r, c);
        *sourceOffset = cell != nullptr ? cell->start : 0;
    }
    return true;
}

QRectF TableObjects::highlightRect(const TableRender& render, const QRectF& area, int from, int to) {
    int row = 0;
    int column = 0;
    if (!render.table.cellOfOffset(from, &row, &column)) return {};
    const TableCellBox* box = render.layout.at(row, column);
    const TableCell* cell = render.table.cellAt(row, column);
    if (box == nullptr || cell == nullptr) return {};
    const QRectF whole = box->rect.translated(area.topLeft());
    // Подстрока — только если показанный текст и есть исходник ячейки (нет
    // разметки); иначе смещения исходника показанному тексту не соответствуют,
    // и честнее подсветить ячейку целиком.
    if (box->text == nullptr || box->text->text() != cell->text || box->text->lineCount() == 0)
        return whole;
    const int a = qBound(0, from - cell->start, int(cell->text.size()));
    const int b = qBound(a, to - cell->start, int(cell->text.size()));
    const ZDocStyle& style = render.style != nullptr ? *render.style : settings().style();
    const qreal padX = tableCellPadX(render.layout.scale, style);
    const qreal padY = tableCellPadY(render.layout.scale, style);
    qreal cx = whole.left() + padX;
    if (box->align == TableAlign::Right)
        cx = whole.right() - padX - box->textWidth;
    else if (box->align == TableAlign::Center)
        cx = whole.left() + (whole.width() - box->textWidth) / 2;
    const QTextLine line = box->text->lineForTextPosition(a);
    if (!line.isValid()) return whole;
    const qreal x1 = line.cursorToX(a);
    const qreal x2 = line.cursorToX(b);
    return QRectF(cx + qMin(x1, x2), whole.top() + padY + line.y(), qAbs(x2 - x1), line.height());
}

// --- обработчик объекта -----------------------------------------------------

TableObjectHandler::TableObjectHandler(NoteView* view) : QObject(view), view_(view) {}

QSizeF TableObjectHandler::intrinsicSize(QTextDocument* doc, int posInDocument,
                                         const QTextFormat& format) {
    Q_UNUSED(format);
    if (view_ == nullptr || doc == nullptr) return {};
    return view_->tableBandFor(doc->findBlock(posInDocument));
}

void TableObjectHandler::drawObject(QPainter* painter, const QRectF& rect, QTextDocument* doc,
                                    int posInDocument, const QTextFormat& format) {
    Q_UNUSED(painter);
    Q_UNUSED(rect);
    Q_UNUSED(doc);
    Q_UNUSED(posInDocument);
    Q_UNUSED(format);
    // Ничего: объект только держит место, сетка ложится поверх готовой
    // страницы (NoteView::paintTableMarks) — выделение Qt кладётся на объект
    // уже после drawObject, и сетка тонула бы в заливке.
}

}  // namespace zametti
