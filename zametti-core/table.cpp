#include "table.h"

namespace zametti {
namespace {

std::string_view trim(std::string_view s) {
    size_t from = 0;
    while (from < s.size() && (s[from] == ' ' || s[from] == '\t')) ++from;
    size_t to = s.size();
    while (to > from && (s[to - 1] == ' ' || s[to - 1] == '\t')) --to;
    return s.substr(from, to - from);
}

std::vector<std::string_view> splitLines(std::string_view text) {
    std::vector<std::string_view> lines;
    size_t start = 0;
    while (start <= text.size()) {
        const size_t end = text.find('\n', start);
        if (end == std::string_view::npos) {
            if (start < text.size()) lines.push_back(text.substr(start));
            break;
        }
        lines.push_back(text.substr(start, end - start));
        start = end + 1;
    }
    return lines;
}

// Ячейки ряда. Палки по краям строки — оформление, а не пустые ячейки:
// "| раз | два |" и "раз | два" дают одно и то же.
//
// Экранированная палка `\|` ряд не рвёт, но И ОБРАТНОЙ КОСОЙ ИЗ ТЕКСТА НЕ
// УБИРАЕТСЯ. Это не мелочь: снятие экранирования — работа разметки, а не
// таблицы, и внутри `кода` его не происходит вовсе. GFM показывает
// "b `\|` az" как код из трёх знаков «\|», и md4c отвечает так же — сверка на
// корпусе gfm на этом и покраснела.
std::vector<std::string> cellsOf(std::string_view line) {
    std::string_view body = trim(line);
    if (!body.empty() && body.front() == '|') body.remove_prefix(1);
    if (!body.empty() && body.back() == '|') {
        // Только если эта палка не экранирована: "a \|" — это ячейка с палкой.
        const bool escaped = body.size() >= 2 && body[body.size() - 2] == '\\';
        if (!escaped) body.remove_suffix(1);
    }

    std::vector<std::string> out;
    std::string current;
    for (size_t i = 0; i < body.size(); ++i) {
        if (body[i] == '\\' && i + 1 < body.size() && body[i + 1] == '|') {
            current += '\\';
            current += '|';
            ++i;
            continue;
        }
        if (body[i] == '|') {
            out.push_back(std::string(trim(current)));
            current.clear();
            continue;
        }
        current += body[i];
    }
    out.push_back(std::string(trim(current)));
    return out;
}

// Строка-разделитель: ячейки вида ---, :---, ---:, :---:. Пустых ячеек в ней
// не бывает, дефис обязателен хотя бы один.
bool delimiterAligns(std::string_view line, std::vector<TableAlign>& align) {
    if (line.find('|') == std::string_view::npos &&
        line.find('-') == std::string_view::npos)
        return false;
    const std::vector<std::string> cells = cellsOf(line);
    if (cells.empty()) return false;

    std::vector<TableAlign> found;
    for (const std::string& raw : cells) {
        std::string_view cell = trim(raw);
        if (cell.empty()) return false;
        const bool left = cell.front() == ':';
        const bool right = cell.back() == ':';
        if (left) cell.remove_prefix(1);
        if (right && !cell.empty()) cell.remove_suffix(1);
        if (cell.empty()) return false;
        if (cell.find_first_not_of('-') != std::string_view::npos) return false;
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
bool startsNewBlock(std::string_view line) {
    std::string_view s = trim(line);
    if (s.empty()) return true;
    if (s.front() == '>') return true;                       // цитата
    if (s.starts_with("```") || s.starts_with("~~~")) return true;   // забор кода

    // Заголовок: от одной до шести решёток и пробел (или конец строки).
    size_t hashes = 0;
    while (hashes < s.size() && s[hashes] == '#') ++hashes;
    if (hashes >= 1 && hashes <= 6 && (hashes == s.size() || s[hashes] == ' '))
        return true;

    // Тематическая черта: три и больше одинаковых знаков из -*_ и пробелы.
    const char mark = s.front();
    if (mark == '-' || mark == '*' || mark == '_') {
        size_t marks = 0;
        bool only = true;
        for (char c : s) {
            if (c == mark) ++marks;
            else if (c != ' ' && c != '\t') { only = false; break; }
        }
        if (only && marks >= 3) return true;
    }

    // Пункт списка: маркер и пробел. Пробел обязателен — "*жир* и текст" не
    // список.
    if ((mark == '-' || mark == '*' || mark == '+') && s.size() > 1 && s[1] == ' ')
        return true;
    size_t digits = 0;
    while (digits < s.size() && s[digits] >= '0' && s[digits] <= '9') ++digits;
    if (digits > 0 && digits + 1 < s.size() && (s[digits] == '.' || s[digits] == ')') &&
        s[digits + 1] == ' ')
        return true;

    return false;
}

}  // namespace

std::string_view Table::cell(int row, int column) const {
    if (row < 0 || row >= int(rows.size())) return {};
    const std::vector<std::string>& line = rows[size_t(row)];
    if (column < 0 || column >= int(line.size())) return {};
    return line[size_t(column)];
}

bool looksLikeTable(std::string_view markdown) {
    const std::vector<std::string_view> lines = splitLines(markdown);
    if (lines.size() < 2) return false;
    if (lines[0].find('|') == std::string_view::npos) return false;
    std::vector<TableAlign> align;
    return delimiterAligns(lines[1], align);
}

Table parseTable(std::string_view markdown) {
    Table table;
    const std::vector<std::string_view> lines = splitLines(markdown);
    if (lines.size() < 2) return table;
    if (lines[0].find('|') == std::string_view::npos) return table;

    std::vector<TableAlign> align;
    if (!delimiterAligns(lines[1], align)) return table;

    // ЧИСЛО КОЛОНОК ЗАДАЁТ СТРОКА-РАЗДЕЛИТЕЛЬ, а не шапка. Спецификация GFM
    // говорит другое — «не совпало, значит не таблица», — но решаем тут не мы:
    // таблицу опознаёт md4c, и в нашем хранилище дословным куском станет
    // ровно то, что он назвал таблицей. Он же на "| abc | def |" с
    // разделителем "| --- |" отвечает «таблица в одну колонку», отрезая
    // лишнюю ячейку шапки. Показ обязан согласиться с ним, иначе кусок,
    // сохранённый как таблица, показывался бы текстом.
    table.columns = int(align.size());
    table.align = align;
    std::vector<std::string> head = cellsOf(lines[0]);
    head.resize(size_t(table.columns));
    table.rows.push_back(std::move(head));

    size_t line = 2;
    for (; line < lines.size(); ++line) {
        // Таблица кончается пустой строкой ИЛИ началом другого блока. Строка
        // без единой палки таблицу не кончает — это ряд из одной ячейки: так
        // говорит и спецификация, и md4c ("| bar | baz |", а следом "bar" —
        // три ряда, не два).
        if (startsNewBlock(lines[line])) break;
        std::vector<std::string> cells = cellsOf(lines[line]);
        cells.resize(size_t(table.columns));
        table.rows.push_back(std::move(cells));
    }

    table.lines = int(line);
    table.valid = true;
    return table;
}

}  // namespace zametti
