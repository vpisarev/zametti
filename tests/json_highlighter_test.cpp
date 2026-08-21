// ZSyntaxHighlighterJSON — подсветка конфига, без виджетов: ключ против
// строки-значения, числа, ключевые слова, комментарий только вне строк,
// экранированная кавычка, пунктуация; цвета — из секции jsonEditing.

#include "json_highlighter.h"
#include "settings.h"
#include "test_util.h"

#include <QTextBlock>
#include <QTextDocument>
#include <QTextLayout>

#include <functional>
#include <string>

namespace {

// Каждый знак отрезка [from, to) строки line накрыт форматом с таким свойством.
bool covered(const QTextDocument& doc, int line, int from, int to,
             const std::function<bool(const QTextCharFormat&)>& test) {
    if (from >= to) return false;
    const QTextBlock block = doc.findBlockByNumber(line);
    if (!block.isValid() || block.layout() == nullptr) return false;
    const QList<QTextLayout::FormatRange> formats = block.layout()->formats();
    for (int at = from; at < to; ++at) {
        bool ok = false;
        for (const QTextLayout::FormatRange& r : formats)
            if (r.start <= at && at < r.start + r.length && test(r.format)) ok = true;
        if (!ok) return false;
    }
    return true;
}

bool untouched(const QTextDocument& doc, int line, int from, int to) {
    const QTextBlock block = doc.findBlockByNumber(line);
    for (const QTextLayout::FormatRange& r : block.layout()->formats())
        if (r.start < to && from < r.start + r.length) return false;
    return true;
}

}  // namespace

TEST(JsonHighlighter, All) {
    const zametti::ZSettings::JsonEditing rules = zametti::settings().jsonEditing();
    QTextDocument doc;
    doc.setPlainText(QStringLiteral(
        "{\n"                                              // 0
        "  \"font\": { \"family\": \"IBM Plex\" },\n"      // 1
        "  \"size\": -12.5e+1, // a comment\n"             // 2
        "  \"url\": \"https://example.com\",\n"            // 3
        "  \"quote\": \"he said \\\"//\\\" ok\",\n"        // 4
        "  \"on\": true, \"off\": false, \"none\": null,\n"   // 5
        "  // whole line\n"                                 // 6
        "  \"truest\": [1, 2]\n"                            // 7
        "}\n"));
    zametti::ZSyntaxHighlighterJSON highlighter(&doc, rules);
    highlighter.rehighlight();

    const auto fg = [](const QColor& want) {
        return [want](const QTextCharFormat& f) { return f.foreground().color() == want; };
    };
    const QString l1 = doc.findBlockByNumber(1).text();
    ZT_TRUE("ключ — цветом ключа", covered(doc, 1, l1.indexOf(QLatin1Char('"')), l1.indexOf(QLatin1Char('"')) + 6, fg(rules.key())));
    ZT_TRUE("вложенный ключ тоже", covered(doc, 1, l1.indexOf(QStringLiteral("\"family\"")), l1.indexOf(QStringLiteral("\"family\"")) + 8, fg(rules.key())));
    const int val = l1.indexOf(QStringLiteral("\"IBM Plex\""));
    ZT_TRUE("строковое значение — цветом строки", covered(doc, 1, val, val + 10, fg(rules.string())));
    ZT_TRUE("а не ключа", !covered(doc, 1, val, val + 10, fg(rules.key())));
    ZT_TRUE("скобки и запятые — пунктуация", covered(doc, 1, l1.indexOf(QLatin1Char('{')), l1.indexOf(QLatin1Char('{')) + 1, fg(rules.punctuation())) && covered(doc, 1, l1.indexOf(QLatin1Char(',')), l1.indexOf(QLatin1Char(',')) + 1, fg(rules.punctuation())));

    const QString l2 = doc.findBlockByNumber(2).text();
    const int num = l2.indexOf(QStringLiteral("-12.5e+1"));
    ZT_TRUE("число с минусом и порядком — целиком", covered(doc, 2, num, num + 8, fg(rules.number())));
    const int com = l2.indexOf(QStringLiteral("//"));
    ZT_TRUE("комментарий до конца строки", covered(doc, 2, com, int(l2.size()), fg(rules.comment())));

    const QString l3 = doc.findBlockByNumber(3).text();
    const int url = l3.indexOf(QStringLiteral("\"https"));
    ZT_TRUE("«//» внутри строки — строка, не комментарий", covered(doc, 3, url, url + 21, fg(rules.string())));
    ZT_TRUE("и не комментарий", !covered(doc, 3, url, url + 21, fg(rules.comment())));

    const QString l4 = doc.findBlockByNumber(4).text();
    const int q = l4.indexOf(QStringLiteral("\"he"));
    const int qend = l4.lastIndexOf(QLatin1Char('"')) + 1;
    ZT_TRUE("экранированная кавычка не закрывает строку", covered(doc, 4, q, qend, fg(rules.string())));

    const QString l5 = doc.findBlockByNumber(5).text();
    for (const char* word : {"true", "false", "null"}) {
        const int at = l5.indexOf(QLatin1String(word));
        ZT_TRUE(std::string(word) + " — ключевое слово", covered(doc, 5, at, at + int(strlen(word)), fg(rules.keyword())));
    }
    ZT_TRUE("строка целиком комментарий", covered(doc, 6, 2, int(doc.findBlockByNumber(6).text().size()), fg(rules.comment())));
    const QString l7 = doc.findBlockByNumber(7).text();
    ZT_TRUE("«truest» в кавычках — ключ, не слово", covered(doc, 7, l7.indexOf(QLatin1Char('"')), l7.indexOf(QLatin1Char('"')) + 8, fg(rules.key())));
    ZT_TRUE("пробелы не красятся", untouched(doc, 7, 0, 2));
}
