#include "json_highlighter.h"

namespace zametti {

ZSyntaxHighlighterJSON::ZSyntaxHighlighterJSON(QTextDocument* document,
                                               ZSettings::JsonEditing rules)
    : QSyntaxHighlighter(document), rules_(std::move(rules)) {
    key_.setForeground(rules_.key());
    string_.setForeground(rules_.string());
    number_.setForeground(rules_.number());
    keyword_.setForeground(rules_.keyword());
    keyword_.setFontWeight(QFont::Bold);
    comment_.setForeground(rules_.comment());
    punctuation_.setForeground(rules_.punctuation());
}

void ZSyntaxHighlighterJSON::highlightBlock(const QString& text) {
    const int n = int(text.size());
    int i = 0;
    while (i < n) {
        const QChar c = text.at(i);
        if (c == QLatin1Char('/') && i + 1 < n && text.at(i + 1) == QLatin1Char('/')) {
            setFormat(i, n - i, comment_);
            return;
        }
        if (c == QLatin1Char('"')) {
            // Строка до закрывающей кавычки; «\"» внутри — не конец. Незакрытая
            // — до конца строки.
            int j = i + 1;
            while (j < n) {
                if (text.at(j) == QLatin1Char('\\')) {
                    j += 2;
                    continue;
                }
                if (text.at(j) == QLatin1Char('"')) {
                    ++j;
                    break;
                }
                ++j;
            }
            const int end = qMin(j, n);
            int k = end;
            while (k < n && text.at(k).isSpace()) ++k;
            const bool isKey = k < n && text.at(k) == QLatin1Char(':');
            setFormat(i, end - i, isKey ? key_ : string_);
            i = end;
            continue;
        }
        if (c.isDigit() || (c == QLatin1Char('-') && i + 1 < n && text.at(i + 1).isDigit())) {
            int j = i + 1;
            while (j < n) {
                const QChar d = text.at(j);
                if (d.isDigit() || d == QLatin1Char('.') || d == QLatin1Char('e') ||
                    d == QLatin1Char('E') || d == QLatin1Char('+') || d == QLatin1Char('-'))
                    ++j;
                else
                    break;
            }
            setFormat(i, j - i, number_);
            i = j;
            continue;
        }
        if (c.isLetter()) {
            int j = i + 1;
            while (j < n && text.at(j).isLetter()) ++j;
            const QString word = text.mid(i, j - i);
            if (word == QLatin1String("true") || word == QLatin1String("false") ||
                word == QLatin1String("null"))
                setFormat(i, j - i, keyword_);
            i = j;
            continue;
        }
        if (c == QLatin1Char('{') || c == QLatin1Char('}') || c == QLatin1Char('[') ||
            c == QLatin1Char(']') || c == QLatin1Char(':') || c == QLatin1Char(',')) {
            setFormat(i, 1, punctuation_);
        }
        ++i;
    }
}

}  // namespace zametti
