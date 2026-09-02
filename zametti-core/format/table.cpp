#include "table.h"

namespace zametti {
namespace {

bool isBlank(QChar ch) { return ch == u' ' || ch == u'\t'; }

// Строка исходника: где начинается и где кончается (без '\n').
struct Line {
    int start = 0;
    int end = 0;
};

std::vector<Line> splitLines(QStringView text) {
    std::vector<Line> lines;
    int start = 0;
    while (start <= text.size()) {
        const qsizetype end = text.indexOf(u'\n', start);
        if (end < 0) {
            if (start < text.size()) lines.push_back({start, int(text.size())});
            break;
        }
        lines.push_back({start, int(end)});
        start = int(end) + 1;
    }
    return lines;
}

// Ячейки ряда со смещениями. Палки по краям строки — оформление, а не пустые
// ячейки: "| раз | два |" и "раз | два" дают одно и то же.
//
// Экранированная палка `\|` ряд не рвёт, но И ОБРАТНОЙ КОСОЙ ИЗ ТЕКСТА НЕ
// УБИРАЕТСЯ. Это не мелочь: снятие экранирования — работа разметки, а не
// таблицы, и внутри `кода` его не происходит вовсе. GFM показывает
// "b `\|` az" как код из трёх знаков «\|», и md4c отвечает так же — сверка на
// корпусе gfm на этом и покраснела.
//
// Ячейка отдаётся без крайних пробелов, и её [start, end) — тоже: смещения
// указывают на сам текст, а не на воздух вокруг него.
std::vector<TableCell> cellsOf(QStringView source, const Line& line) {
    int from = line.start;
    int to = line.end;
    while (from < to && isBlank(source.at(from))) ++from;
    while (to > from && isBlank(source.at(to - 1))) --to;
    if (from < to && source.at(from) == u'|') ++from;
    if (to > from && source.at(to - 1) == u'|') {
        // Только если эта палка не экранирована: "a \|" — это ячейка с палкой.
        const bool escaped = to - 2 >= from && source.at(to - 2) == u'\\';
        if (!escaped) --to;
    }

    std::vector<TableCell> out;
    const auto push = [&](int cellFrom, int cellTo) {
        while (cellFrom < cellTo && isBlank(source.at(cellFrom))) ++cellFrom;
        while (cellTo > cellFrom && isBlank(source.at(cellTo - 1))) --cellTo;
        TableCell cell;
        cell.start = cellFrom;
        cell.end = cellTo;
        cell.text = source.mid(cellFrom, cellTo - cellFrom).toString();
        out.push_back(std::move(cell));
    };
    int cellFrom = from;
    for (int i = from; i < to; ++i) {
        const QChar ch = source.at(i);
        if (ch == u'\\' && i + 1 < to && source.at(i + 1) == u'|') {
            ++i;
            continue;
        }
        if (ch == u'|') {
            push(cellFrom, i);
            cellFrom = i + 1;
        }
    }
    push(cellFrom, to);
    return out;
}

// Строка-разделитель: ячейки вида ---, :---, ---:, :---:. Пустых ячеек в ней
// не бывает, дефис обязателен хотя бы один.
bool delimiterAligns(QStringView source, const Line& line, std::vector<TableAlign>& align) {
    const QStringView text = source.mid(line.start, line.end - line.start);
    if (text.indexOf(u'|') < 0 && text.indexOf(u'-') < 0) return false;
    const std::vector<TableCell> cells = cellsOf(source, line);
    if (cells.empty()) return false;

    std::vector<TableAlign> found;
    for (const TableCell& raw : cells) {
        QStringView cell = raw.text;
        if (cell.isEmpty()) return false;
        const bool left = cell.front() == u':';
        const bool right = cell.back() == u':';
        if (left) cell = cell.mid(1);
        if (right && !cell.isEmpty()) cell.chop(1);
        if (cell.isEmpty()) return false;
        for (const QChar ch : cell)
            if (ch != u'-') return false;
        found.push_back(left && right  ? TableAlign::Center
                        : right        ? TableAlign::Right
                        : left         ? TableAlign::Left
                                       : TableAlign::Default);
    }
    align = found;
    return true;
}

// Начинает ли эта строка ДРУГОЙ блок. Такая строка кончает таблицу — так
// говорит спецификация («таблица ломается пустой строкой или началом другого
// блока») и так же ведёт себя md4c: после "| bar | baz |" строка "> bar"
// оставляет в теле один ряд, а не два. Сверка на корпусе gfm на этом и
// покраснела.
//
// Набор признаков намеренно маленький: полный разбор блоков живёт в md4c, а
// здесь нужно ровно то, что встречается сразу за таблицей. Что этого хватает,
// говорит та же сверка — на полутора тысячах файлов корпусов расхождений нет.
bool startsNewBlock(QStringView s) {
    while (!s.isEmpty() && isBlank(s.front())) s = s.mid(1);
    while (!s.isEmpty() && isBlank(s.back())) s.chop(1);
    if (s.isEmpty()) return true;
    if (s.front() == u'>') return true;                                              // цитата
    if (s.startsWith(u"```") || s.startsWith(u"~~~")) return true;                     // забор кода

    // Заголовок: от одной до шести решёток и пробел (или конец строки).
    int hashes = 0;
    while (hashes < s.size() && s.at(hashes) == u'#') ++hashes;
    if (hashes >= 1 && hashes <= 6 && (hashes == s.size() || s.at(hashes) == u' '))
        return true;

    // Тематическая черта: три и больше одинаковых знаков из -*_ и пробелы.
    const QChar mark = s.front();
    if (mark == u'-' || mark == u'*' || mark == u'_') {
        int marks = 0;
        bool only = true;
        for (const QChar c : s) {
            if (c == mark) ++marks;
            else if (!isBlank(c)) { only = false; break; }
        }
        if (only && marks >= 3) return true;
    }

    // Пункт списка: маркер и пробел. Пробел обязателен — "*жир* и текст" не
    // список.
    if ((mark == u'-' || mark == u'*' || mark == u'+') && s.size() > 1 && s.at(1) == u' ')
        return true;
    int digits = 0;
    while (digits < s.size() && s.at(digits) >= u'0' && s.at(digits) <= u'9') ++digits;
    if (digits > 0 && digits + 1 < s.size() && (s.at(digits) == u'.' || s.at(digits) == u')') &&
        s.at(digits + 1) == u' ')
        return true;

    return false;
}

}  // namespace

std::vector<TableCell> rowCells(QStringView row) {
    return cellsOf(row, Line{0, int(row.size())});
}

QStringView Table::cell(int row, int column) const {
    const TableCell* found = cellAt(row, column);
    return found == nullptr ? QStringView() : QStringView(found->text);
}

const TableCell* Table::cellAt(int row, int column) const {
    if (row < 0 || row >= int(rows.size())) return nullptr;
    const std::vector<TableCell>& line = rows[size_t(row)];
    if (column < 0 || column >= int(line.size())) return nullptr;
    return &line[size_t(column)];
}

bool Table::cellOfOffset(int offset, int* row, int* column) const {
    for (size_t r = 0; r < rows.size(); ++r) {
        const std::vector<TableCell>& line = rows[r];
        if (line.empty()) continue;
        // Ряд занимает исходник от первой своей ячейки до последней; ячейки
        // идут слева направо, и смещение между ними принадлежит той, что
        // начинается раньше него последней.
        if (offset < line.front().start - 1 || offset > line.back().end + 1) continue;
        size_t best = 0;
        for (size_t c = 0; c < line.size(); ++c)
            if (line[c].start <= offset) best = c;
        if (row != nullptr) *row = int(r);
        if (column != nullptr) *column = int(best);
        return true;
    }
    return false;
}

bool looksLikeTable(QStringView markdown) {
    const std::vector<Line> lines = splitLines(markdown);
    if (lines.size() < 2) return false;
    if (markdown.mid(lines[0].start, lines[0].end - lines[0].start).indexOf(u'|') < 0) return false;
    std::vector<TableAlign> align;
    return delimiterAligns(markdown, lines[1], align);
}

Table parseTable(QStringView markdown) {
    Table table;
    const std::vector<Line> lines = splitLines(markdown);
    if (lines.size() < 2) return table;
    if (markdown.mid(lines[0].start, lines[0].end - lines[0].start).indexOf(u'|') < 0) return table;

    std::vector<TableAlign> align;
    if (!delimiterAligns(markdown, lines[1], align)) return table;

    // ЧИСЛО КОЛОНОК ЗАДАЁТ СТРОКА-РАЗДЕЛИТЕЛЬ, а не шапка. Спецификация GFM
    // говорит другое — «не совпало, значит не таблица», — но решаем тут не мы:
    // таблицу опознаёт md4c, и в нашем хранилище дословным куском станет
    // ровно то, что он назвал таблицей. Он же на "| abc | def |" с
    // разделителем "| --- |" отвечает «таблица в одну колонку», отрезая
    // лишнюю ячейку шапки. Показ обязан согласиться с ним, иначе кусок,
    // сохранённый как таблица, показывался бы текстом.
    table.columns = int(align.size());
    table.align = align;

    // Ряд по разделителю: лишние ячейки отрезаются, недостающие — пустые, и
    // стоят они там, где стояли бы: в конце ряда.
    const auto fitted = [&](std::vector<TableCell> cells) {
        const int tail = cells.empty() ? 0 : cells.back().end;
        cells.resize(size_t(table.columns));
        for (TableCell& cell : cells)
            if (cell.text.isEmpty() && cell.start == 0 && cell.end == 0) cell.start = cell.end = tail;
        return cells;
    };
    table.rows.push_back(fitted(cellsOf(markdown, lines[0])));

    size_t line = 2;
    for (; line < lines.size(); ++line) {
        // Таблица кончается пустой строкой ИЛИ началом другого блока. Строка
        // без единой палки таблицу не кончает — это ряд из одной ячейки: так
        // говорит и спецификация, и md4c ("| bar | baz |", а следом "bar" —
        // три ряда, не два).
        if (startsNewBlock(markdown.mid(lines[line].start, lines[line].end - lines[line].start))) break;
        table.rows.push_back(fitted(cellsOf(markdown, lines[line])));
    }

    table.lines = int(line);
    table.valid = true;
    return table;
}

}  // namespace zametti
