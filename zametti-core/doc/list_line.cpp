#include "list_line.h"

namespace zametti {

namespace {

bool isIndentChar(QChar c) { return c == QLatin1Char(' ') || c == QLatin1Char('\t'); }

bool isBulletChar(QChar c) {
    return c == QLatin1Char('-') || c == QLatin1Char('*') || c == QLatin1Char('+');
}

// Пробельный знак за маркером — то, что CommonMark зовёт whitespace после
// маркера; перевод строки сюда не попадает (строка уже без него).
bool isMarkerGap(QChar c) { return c.isSpace(); }

}  // namespace

int leadingWhitespace(QStringView line) {
    int n = 0;
    while (n < line.size() && isIndentChar(line.at(n))) ++n;
    return n;
}

int columnOf(QStringView line, int chars, int tabStop) {
    const int stop = qMax(1, tabStop);
    int column = 0;
    const int end = qMin(chars, int(line.size()));
    for (int i = 0; i < end; ++i) {
        if (line.at(i) == QLatin1Char('\t'))
            column += stop - column % stop;
        else
            ++column;
    }
    return column;
}

bool isBlankLine(QStringView line) { return leadingWhitespace(line) == line.size(); }

ListLine parseListLine(QStringView line, int tabStop) {
    ListLine out;
    const int n = int(line.size());
    out.indentChars = leadingWhitespace(line);
    out.indent = columnOf(line, out.indentChars, tabStop);
    int p = out.indentChars;
    if (p >= n) return out;

    int markerWidth = 0;   // ширина маркера в колонках: буллет 1, номер цифры+1
    if (isBulletChar(line.at(p))) {
        out.bullet = line.at(p);
        markerWidth = 1;
        int q = p + 1;
        // ЗАДАЧА раньше буллета: `- [ ]` и краткая `-[ ]`.
        int box = q;
        if (box < n && line.at(box) == QLatin1Char(' ')) ++box;
        if (box + 2 < n && line.at(box) == QLatin1Char('[') &&
            (line.at(box + 1) == QLatin1Char(' ') || line.at(box + 1) == QLatin1Char('x') ||
             line.at(box + 1) == QLatin1Char('X')) &&
            line.at(box + 2) == QLatin1Char(']') &&
            (box + 3 >= n || isMarkerGap(line.at(box + 3)))) {
            out.item = true;
            out.marker = Marker::Task;
            out.checked = line.at(box + 1) != QLatin1Char(' ');
            out.markerEnd = box + 3;
        } else if (q < n && isMarkerGap(line.at(q))) {
            out.item = true;
            out.marker = Marker::Bullet;
            out.markerEnd = q;
        } else {
            return out;
        }
    } else if (line.at(p).isDigit()) {
        int q = p;
        int value = 0;
        while (q < n && q - p < 9 && line.at(q) >= QLatin1Char('0') && line.at(q) <= QLatin1Char('9')) {
            value = value * 10 + (line.at(q).unicode() - '0');
            ++q;
        }
        // Десятая цифра подряд — не номер (как `\d{1,9}` у md4c).
        if (q >= n || (line.at(q) != QLatin1Char('.') && line.at(q) != QLatin1Char(')'))) return out;
        if (q + 1 >= n || !isMarkerGap(line.at(q + 1))) return out;
        out.item = true;
        out.marker = Marker::Ordered;
        out.ordinal = value;
        out.delimiter = line.at(q);
        out.markerEnd = q + 1;
        markerWidth = q + 1 - p;
    } else {
        return out;
    }

    out.contentStart = out.markerEnd < n && isMarkerGap(line.at(out.markerEnd)) ? out.markerEnd + 1
                                                                                : out.markerEnd;
    out.contentColumn = out.indent + markerWidth + 1;
    out.emptyBody = isBlankLine(line.mid(out.contentStart));
    return out;
}

QString nextMarker(const ListLine& line) {
    if (!line.item) return {};
    switch (line.marker) {
        case Marker::Bullet:
            return QString(line.bullet) + QLatin1Char(' ');
        case Marker::Ordered:
            return QString::number(line.ordinal + 1) + line.delimiter + QLatin1Char(' ');
        case Marker::Task:
            return QString(line.bullet) + QStringLiteral(" [ ] ");
    }
    return {};
}

}  // namespace zametti
