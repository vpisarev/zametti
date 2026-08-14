#include "table_view.h"

#include "doc_model.h"
#include "document_pieces.h"
#include "note_header.h"

#include "settings.h"

#include <QFontMetricsF>
#include <QTextOption>

#include <algorithm>
#include <cmath>

namespace zametti {
namespace {

// Кегль кода внутри ячейки — по тому же правилу, что и везде: ступень от
// кегля текста. Таблицу рисуем мы сами, ступеней у QPainter нет, поэтому
// множитель ступени берём числом.
qreal codePointFor(qreal scale) {
    return appearance().baseFontPoint * fontStepFactor(appearance().codeStep) * scale;
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
    std::vector<Piece> blocks;
    NoteHeader header;
    parsePieces(markdown, blocks, header);
    if (blocks.empty()) return out;

    const Piece& block = blocks.front();
    out.text = QString::fromUtf8(block.text.data(), qsizetype(block.text.size()));
    // Переводов строк внутри ячейки в GFM не бывает, но дословный кусок мог
    // принести что угодно: рисуем пробелом, чтобы не рвать разметку.
    out.text.replace(QLatin1Char('\n'), QLatin1Char(' '));

    // Смещения кусков заданы в БАЙТАХ UTF-8, а QString считает в кодовых
    // единицах UTF-16: приравнивать их нельзя, ошибка вылезет на первом же
    // не-ASCII (весь корпус владельца — русский).
    const auto utf16At = [&block](int byteOffset) {
        const size_t at = size_t(std::clamp(byteOffset, 0, int(block.text.size())));
        return int(QString::fromUtf8(block.text.data(), qsizetype(at)).size());
    };

    for (const Run& run : block.runs) {
        if (run.empty()) continue;
        CellMarkup::Span piece;
        piece.start = utf16At(run.start);
        piece.length = utf16At(run.end) - piece.start;
        if (piece.length <= 0) continue;
        piece.bold = run.bold();
        piece.italic = run.italic();
        piece.strike = run.strike();
        piece.code = run.code();
        piece.link = !run.href.empty();
        out.spans.push_back(piece);
    }
    return out;
}

// Разметка ячейки при этом шрифте. Разбор уже сделан — здесь только форматы.
std::shared_ptr<QTextLayout> layoutOfCell(const CellMarkup& markup, const QFont& font,
                                          qreal scale) {
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
            fmt.setFontPointSize(codePointFor(scale));
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
// РАСКЛАДЫВАЕМ СРАЗУ НАБЕЛО. Первая редакция мерила ширину метриками шрифта, а
// потом раскладывала ячейку заново — то есть шейпила каждую ячейку дважды.
// Замер (50×8, 400 ячеек): завести QTextLayout 42 мкс, разложить все один раз
// 1152, вся раскладка 2442 — ровно два прохода. Теперь проход один: ячейка
// раскладывается в бесконечную ширину, и если колонке её ширины хватило,
// раскладка так и остаётся годной; переразложить надо только те ячейки,
// которым досталось меньше запрошенного.
qreal naturalWidth(QTextLayout& layout, qreal lineHeight) {
    layout.beginLayout();
    qreal width = 0.0;
    qreal y = 0.0;
    for (QTextLine line = layout.createLine(); line.isValid(); line = layout.createLine()) {
        line.setLineWidth(1e6);   // без переноса: спрашиваем естественную ширину
        line.setPosition(QPointF(0, y));
        width = qMax(width, line.naturalTextWidth());
        y += lineHeight;
    }
    layout.endLayout();
    return width;
}

// Сколько ячейке нужно КАК МИНИМУМ, чтобы не рвать слова: ширина самого
// длинного слова.
//
// Это и есть мера, которой не хватало первой редакции. Она мерила только
// «сколько ячейка просит одной строкой», и одна длинная ячейка раздувала свою
// колонку до упора, а лечилось это усадкой шрифта ВСЕЙ таблицы — не тем
// лекарством от не той болезни (замечание владельца). Зная минимум, ширину
// колонки можно выбрать между минимумом и запросом, а текст перенести по
// словам — так делают и браузеры, и GitHub.
qreal longestWordWidth(const QString& text, const QFont& font) {
    const QFontMetricsF metrics(font);
    qreal widest = 0.0;
    for (const QString& word : text.split(QLatin1Char(' '), Qt::SkipEmptyParts))
        widest = qMax(widest, metrics.horizontalAdvance(word));
    return widest;
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

QFont tableFont(qreal scale) {
    QFont font{QString(appearance().fontFamily)};
    font.setPointSizeF(appearance().baseFontPoint * scale);
    font.setStyleHint(QFont::Monospace);
    return font;
}

qreal tableCellPadX(qreal scale) {
    // Поля ячейки — от кегля, а не в пикселях: с зумом и с усадкой они едут
    // вместе с текстом, иначе ужатая таблица стоит в непропорционально
    // просторных клетках.
    return QFontMetricsF(tableFont(scale)).horizontalAdvance(QLatin1Char('A')) *
           appearance().tables.cellPadding;
}

qreal tableCellPadY(qreal scale) {
    return QFontMetricsF(tableFont(scale)).height() * appearance().tables.cellPaddingY;
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
    const qreal room = qMax(space.columnWidth, space.fullWidth);

    // Пол усадки — ниже него шрифт не ужимаем; вместо этого разрешаем рвать
    // слова где угодно.
    constexpr qreal kFloor = 0.55;

    // Разбор ячеек — ОДИН РАЗ: от масштаба он не зависит.
    QVector<CellMarkup> markup;
    markup.reserve(out.rows * out.columns);
    for (int row = 0; row < out.rows; ++row)
        for (int column = 0; column < out.columns; ++column)
            markup.push_back(markupOfCell(table.cell(row, column)));

    // Мера колонки при этом масштабе: min (самый длинный неразрывный токен) и
    // max (естественная ширина без переносов) — как в RFC 1942.
    struct Measure {
        QVector<std::shared_ptr<QTextLayout>> layouts;
        QVector<qreal> cellWidth;   // естественная ширина КАЖДОЙ ячейки
        QVector<qreal> minWidth;
        QVector<qreal> maxWidth;
        qreal minTotal = 0.0;
        qreal maxTotal = 0.0;
        qreal padX = 0.0;
        qreal padY = 0.0;
        qreal lineHeight = 0.0;
    };
    const auto measure = [&](qreal scale) {
        Measure m;
        const QFont font = tableFont(scale);
        m.padX = tableCellPadX(scale);
        m.padY = tableCellPadY(scale);
        m.lineHeight = std::round(QFontMetricsF(font).height() * appearance().lineHeightFactor);
        m.minWidth.fill(0.0, out.columns);
        m.maxWidth.fill(0.0, out.columns);
        m.layouts.reserve(out.rows * out.columns);
        m.cellWidth.reserve(out.rows * out.columns);
        for (int row = 0; row < out.rows; ++row) {
            for (int column = 0; column < out.columns; ++column) {
                QFont cellFont = font;
                if (row == 0) cellFont.setBold(true);   // шапка
                const CellMarkup& cell = markup[row * out.columns + column];
                auto layout = layoutOfCell(cell, cellFont, scale);
                const qreal natural = naturalWidth(*layout, m.lineHeight);
                m.cellWidth.push_back(natural);
                m.maxWidth[column] = qMax(m.maxWidth[column], natural + 2 * m.padX);
                m.layouts.push_back(layout);
            }
        }
        for (qreal w : m.maxWidth) m.maxTotal += w;
        return m;
    };

    // Минимумы считаются ОТДЕЛЬНО и только если понадобятся: измерение самого
    // длинного слова — это шейпинг каждого слова каждой ячейки, и на таблице
    // 50×8 оно стоило 1.3 мс из 3.9 (замер). Пока Σmax помещается, минимумы не
    // нужны вовсе — а помещается оно у всех таблиц корпуса, кроме самых широких.
    const auto measureMins = [&](Measure& m, qreal scale) {
        if (m.minTotal > 0.0) return;
        const QFont font = tableFont(scale);
        for (int row = 0; row < out.rows; ++row) {
            for (int column = 0; column < out.columns; ++column) {
                QFont cellFont = font;
                if (row == 0) cellFont.setBold(true);
                const CellMarkup& cell = markup[row * out.columns + column];
                m.minWidth[column] = qMax(m.minWidth[column],
                                          longestWordWidth(cell.text, cellFont) + 2 * m.padX);
            }
        }
        for (int i = 0; i < out.columns; ++i) {
            m.minWidth[i] = qMin(m.minWidth[i], m.maxWidth[i]);   // min не больше max
            m.minTotal += m.minWidth[i];
        }
    };

    // АЛГОРИТМ АВТО-РАСКЛАДКИ CSS2/HTML4 (RFC 1942) — тот, что четверть века
    // стоит в каждом браузере:
    //
    //   Σmax ≤ W          — всем max, переносов нет вовсе;
    //   Σmin ≤ W < Σmax   — каждой min плюс доля остатка ∝ (max − min);
    //   Σmin > W          — каскад широких таблиц: поля (уже учтены в W),
    //                       усадка шрифта, пол и разрыв слов где угодно.
    //
    // Из второго правила само собой следует то, чего и хотелось: переносится
    // ровно та колонка, которой досталось меньше её max, а колонки с min == max
    // (числа, даты, короткие слова) не переносятся никогда.
    Measure m = measure(1.0);
    qreal scale = 1.0;
    bool wrapped = false;
    QVector<qreal> widths;

    const auto distribute = [&](const Measure& at) {
        QVector<qreal> result(out.columns, 0.0);
        const qreal spare = room - at.minTotal;
        const qreal hunger = at.maxTotal - at.minTotal;
        for (int i = 0; i < out.columns; ++i)
            result[i] = at.minWidth[i] +
                        (hunger > 0 ? (at.maxWidth[i] - at.minWidth[i]) * spare / hunger : 0.0);
        return result;
    };

    if (m.maxTotal <= room) {
        widths = m.maxWidth;
    } else if (measureMins(m, 1.0), m.minTotal <= room) {
        widths = distribute(m);
        wrapped = true;
    } else {
        // УСАДКА ОДНИМ ДЕЛЕНИЕМ, а не перебором с шагом. min и max линейны по
        // кеглю, значит нужный масштаб считается сразу: Σmin × scale = W.
        // Перебор стоил бы десяти полных раскладок таблицы (2.5 мс каждая на
        // 50×8) ровно там, где таблица и так самая тяжёлая.
        scale = qBound(kFloor, room / m.minTotal, 1.0);
        m = measure(scale);
        measureMins(m, scale);
        if (m.maxTotal <= room) {
            widths = m.maxWidth;
        } else if (m.minTotal <= room) {
            widths = distribute(m);
            wrapped = true;
        } else {
            // Пол усадки: слова-монстры (URL, длинные идентификаторы) режем где
            // угодно — min схлопывается, и вписывание гарантировано.
            //
            // Раздаём место ТОЧНО ПО МЕСТУ, а не «каждой не меньше чем...»:
            // нижняя граница на колонку в сумме легко перерастает всю ширину,
            // и таблица вылезала за край ровно там, где обязана была вписаться
            // любой ценой (проверка на 90 пикселей это и поймала).
            const qreal minCell = 2 * m.padX + 1.0;
            widths.fill(0.0, out.columns);
            if (minCell * out.columns >= room) {
                for (int i = 0; i < out.columns; ++i) widths[i] = room / out.columns;
            } else {
                const qreal spare = room - minCell * out.columns;
                for (int i = 0; i < out.columns; ++i)
                    widths[i] = minCell + (m.maxTotal > 0 ? m.maxWidth[i] / m.maxTotal * spare : 0);
            }
            wrapped = true;
        }
    }

    QVector<qreal> rowHeights(out.rows, 0.0);
    for (int row = 0; row < out.rows; ++row) {
        qreal tallest = 0.0;
        for (int column = 0; column < out.columns; ++column) {
            const int index = row * out.columns + column;
            QTextLayout& layout = *m.layouts[index];
            const qreal inner = widths[column] - 2 * m.padX;
            // Ячейке хватило — её раскладка уже сделана и годна. Переразложить
            // надо только те, которым досталось меньше запрошенного: это и
            // есть второй проход, и теперь он идёт не по всем ячейкам, а по
            // переносимым.
            const qreal text = m.cellWidth[index] <= inner + 0.5
                                   ? qMax(m.lineHeight, layout.boundingRect().height())
                                   : layoutInto(layout, inner, m.lineHeight);
            tallest = qMax(tallest, text + 2 * m.padY);
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
            box.text = m.layouts[row * out.columns + column];
            box.textWidth = qMin(m.cellWidth[row * out.columns + column],
                                 widths[column] - 2 * m.padX);
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

}  // namespace zametti
