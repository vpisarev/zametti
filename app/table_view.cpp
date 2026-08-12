#include "table_view.h"

#include "ir.h"
#include "parser.h"
#include "settings.h"

#include <QFontMetricsF>
#include <QTextOption>

#include <algorithm>
#include <cmath>

namespace zametti {
namespace {

// Кегль кода внутри ячейки — по тому же правилу, что и везде: величина
// абсолютная, а не доля от окружающего текста.
qreal codePointFor(qreal zoom, qreal scale) {
    const qreal code = appearance().codePointSize;
    const qreal base = appearance().baseFontPoint;
    return (code > 0.0 ? code : base) * zoom * scale;
}

// Разобранная ячейка: текст и куски разметки, БЕЗ шрифтов и размеров.
//
// Разбор от масштаба не зависит, а усадка перебирает масштабы по кругу — и
// первая редакция звала parse() внутри этого круга. Разбираем один раз, по
// кругу ходят только шрифты.
//
// Сколько это стоит на самом деле (замер, таблица 50×8, 400 ячеек): разбор
// самой таблицы 36 мкс, разбор всех ячеек 211 мкс, вся раскладка 2585 мкс.
// То есть разбор — восьмая часть, а остальное — раскладка текста самой Qt.
// Догадка «почти всё уходит на разбор» замером НЕ подтвердилась, и хорошо,
// что я её проверил, прежде чем записывать в комментарий.
struct CellMarkup {
    QString text;
    struct Span {
        int start = 0;
        int length = 0;
        bool bold = false;
        bool italic = false;
        bool strike = false;
        bool code = false;
        bool link = false;
    };
    QVector<Span> spans;
};

CellMarkup markupOfCell(std::string_view markdown) {
    CellMarkup out;
    const Document ir = parse(std::string(markdown));
    if (ir.blocks.empty()) return out;

    const Block& block = ir.blocks.front();
    const std::string_view body = ir.text(block);
    out.text = QString::fromUtf8(body.data(), qsizetype(body.size()));
    // Переводов строк внутри ячейки в GFM не бывает, но дословный кусок мог
    // принести что угодно: рисуем пробелом, чтобы не рвать разметку.
    out.text.replace(QLatin1Char('\n'), QLatin1Char(' '));

    // Смещения спанов заданы в БАЙТАХ UTF-8, а QString считает в кодовых
    // единицах UTF-16: приравнивать их нельзя, ошибка вылезет на первом же
    // не-ASCII (весь корпус владельца — русский).
    const auto utf16At = [&body](int byteOffset) {
        const size_t at = size_t(std::clamp(byteOffset, 0, int(body.size())));
        return int(QString::fromUtf8(body.data(), qsizetype(at)).size());
    };

    for (const Inline& span : ir.inlines(block)) {
        if (span.text.size() <= 0) continue;
        CellMarkup::Span piece;
        piece.start = utf16At(span.text.start);
        piece.length = utf16At(span.text.end) - piece.start;
        if (piece.length <= 0) continue;
        piece.bold = span.bold();
        piece.italic = span.italic();
        piece.strike = span.strike();
        piece.code = span.code();
        piece.link = !span.href.empty();
        out.spans.push_back(piece);
    }
    return out;
}

// Разметка ячейки при этом шрифте. Разбор уже сделан — здесь только форматы.
std::shared_ptr<QTextLayout> layoutOfCell(const CellMarkup& markup, const QFont& font,
                                          qreal zoom, qreal scale) {
    QList<QTextLayout::FormatRange> formats;
    for (const CellMarkup::Span& piece : markup.spans) {
        QTextLayout::FormatRange range;
        range.start = piece.start;
        range.length = piece.length;

        QTextCharFormat fmt;
        fmt.setFont(font);
        if (piece.bold) fmt.setFontWeight(QFont::Bold);
        if (piece.italic) fmt.setFontItalic(true);
        if (piece.strike) fmt.setFontStrikeOut(true);
        if (piece.code) {
            fmt.setBackground(appearance().codeBackground);
            fmt.setFontPointSize(codePointFor(zoom, scale));
            if (!appearance().codeFamily.isEmpty())
                fmt.setFontFamilies({QString(appearance().codeFamily)});
        }
        if (piece.link) {
            fmt.setForeground(appearance().linkColor);
            fmt.setFontUnderline(true);
        }
        range.format = fmt;
        formats.append(range);
    }

    auto layout = std::make_shared<QTextLayout>(markup.text, font);
    layout->setFormats(formats);
    QTextOption option;
    option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    layout->setTextOption(option);
    return layout;
}

// Ширина ячейки без переноса — то, сколько она просит.
//
// У ячейки БЕЗ разметки спрашиваем метрики шрифта, а не раскладку: раскладка
// текста — это почти вся цена таблицы (см. замер выше), и делать её дважды
// (сперва ради ширины, потом ради показа) незачем. Метрики дают сумму
// продвижений без кернинга, то есть чуть больше или столько же; лишний
// пиксель ширины колонки безвреден, а нехватка вызвала бы ложный перенос.
//
// Ячейка с разметкой считается раскладкой честно: у кусков разные шрифты, и
// суммой одной гарнитуры её не измерить.
qreal naturalWidth(QTextLayout& layout, bool plain) {
    if (plain) return QFontMetricsF(layout.font()).horizontalAdvance(layout.text());
    layout.beginLayout();
    QTextLine line = layout.createLine();
    qreal width = 0.0;
    while (line.isValid()) {
        line.setLineWidth(1e6);   // без переноса: спрашиваем естественную ширину
        width = qMax(width, line.naturalTextWidth());
        line = layout.createLine();
    }
    layout.endLayout();
    return width;
}

// Разложить ячейку в заданную ширину и вернуть высоту текста.
qreal layoutInto(QTextLayout& layout, qreal width, qreal lineHeight) {
    layout.beginLayout();
    qreal y = 0.0;
    for (QTextLine line = layout.createLine(); line.isValid(); line = layout.createLine()) {
        line.setLineWidth(qMax(1.0, width));
        line.setPosition(QPointF(0, y));
        y += lineHeight;
    }
    layout.endLayout();
    return qMax(lineHeight, y);
}

}  // namespace

QFont tableFont(qreal zoom, qreal scale) {
    QFont font{QString(appearance().fontFamily)};
    font.setPointSizeF(appearance().baseFontPoint * zoom * scale);
    font.setStyleHint(QFont::Monospace);
    return font;
}

qreal tableCellPadX(qreal zoom, qreal scale) {
    // Поля ячейки — от кегля, а не в пикселях: с зумом и с усадкой они едут
    // вместе с текстом, иначе ужатая таблица стоит в непропорционально
    // просторных клетках.
    return QFontMetricsF(tableFont(zoom, scale)).horizontalAdvance(QLatin1Char('A')) * 0.6;
}

qreal tableCellPadY(qreal zoom, qreal scale) {
    return QFontMetricsF(tableFont(zoom, scale)).height() * 0.25;
}

const TableCellBox* TableLayout::at(int row, int column) const {
    if (row < 0 || column < 0 || row >= rows || column >= columns) return nullptr;
    const int index = row * columns + column;
    return index < cells.size() ? &cells[index] : nullptr;
}

TableLayout layoutTable(const Table& table, const TableSpace& space) {
    TableLayout out;
    if (!table.valid || table.columns <= 0) return out;

    out.columns = table.columns;
    out.rows = int(table.rows.size());

    // Место, в которое вписываемся: сперва колонка текста, а если не влезли —
    // поля до ширины окна (правило владельца «как картинки»).
    const qreal roomy = qMax(space.columnWidth, space.fullWidth);

    // Пол усадки — ниже него шрифт не ужимаем, включается перенос.
    constexpr qreal kFloor = 0.55;
    constexpr qreal kStep = 0.05;

    // Разбор ячеек — ОДИН РАЗ на всю раскладку, до круга усадки: от масштаба
    // он не зависит, а стоит почти всю её цену (см. замер выше по файлу).
    QVector<CellMarkup> markup;
    markup.reserve(out.rows * out.columns);
    for (int row = 0; row < out.rows; ++row)
        for (int column = 0; column < out.columns; ++column)
            markup.push_back(markupOfCell(table.cell(row, column)));

    for (qreal scale = 1.0;; scale -= kStep) {
        const bool floored = scale <= kFloor;
        if (floored) scale = kFloor;

        const QFont font = tableFont(space.zoom, scale);
        const qreal padX = tableCellPadX(space.zoom, scale);
        const qreal padY = tableCellPadY(space.zoom, scale);
        const qreal lineHeight =
            std::round(QFontMetricsF(font).height() * appearance().lineHeightFactor);

        // Разметка каждой ячейки и её естественная ширина.
        QVector<std::shared_ptr<QTextLayout>> layouts;
        QVector<qreal> wants(out.columns, 0.0);
        layouts.reserve(out.rows * out.columns);
        for (int row = 0; row < out.rows; ++row) {
            for (int column = 0; column < out.columns; ++column) {
                QFont cellFont = font;
                if (row == 0) cellFont.setBold(true);   // шапка
                auto layout = layoutOfCell(markup[row * out.columns + column], cellFont,
                                           space.zoom, scale);
                const bool plain = markup[row * out.columns + column].spans.isEmpty();
                wants[column] = qMax(wants[column], naturalWidth(*layout, plain) + 2 * padX);
                layouts.push_back(layout);
            }
        }

        qreal total = 0.0;
        for (qreal w : wants) total += w;

        // Влезли — раскладываем и уходим. Не влезли и пол не достигнут —
        // ужимаем ещё. Достигли пола — раскладываем с переносом: ширины
        // колонок ужимаются пропорционально их запросам.
        QVector<qreal> widths = wants;
        bool wrapped = false;
        if (total > roomy) {
            if (!floored) continue;
            const qreal factor = roomy / total;
            for (qreal& w : widths) w = qMax(4 * padX, w * factor);
            wrapped = true;
        }

        QVector<qreal> rowHeights(out.rows, 0.0);
        for (int row = 0; row < out.rows; ++row) {
            qreal tallest = 0.0;
            for (int column = 0; column < out.columns; ++column) {
                QTextLayout& layout = *layouts[row * out.columns + column];
                const qreal text = layoutInto(layout, widths[column] - 2 * padX, lineHeight);
                tallest = qMax(tallest, text + 2 * padY);
            }
            rowHeights[row] = std::round(tallest);
        }

        qreal x = 0.0;
        qreal y = 0.0;
        out.cells.reserve(out.rows * out.columns);
        for (int row = 0; row < out.rows; ++row) {
            x = 0.0;
            for (int column = 0; column < out.columns; ++column) {
                TableCellBox box;
                box.rect = QRectF(x, y, widths[column], rowHeights[row]);
                box.text = layouts[row * out.columns + column];
                box.align = column < int(table.align.size()) ? table.align[size_t(column)]
                                                             : TableAlign::Default;
                box.row = row;
                box.column = column;
                out.cells.push_back(box);
                x += widths[column];
            }
            y += rowHeights[row];
        }

        out.columnWidth = widths;
        out.rowHeight = rowHeights;
        out.width = x;
        out.height = y;
        out.scale = scale;
        out.wrapped = wrapped;
        return out;
    }
}

}  // namespace zametti
