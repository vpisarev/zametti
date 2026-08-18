// ZSyntaxHighlighterMD — подсветка сырого markdown, без единого виджета.
//
// Документ — ZDocument::fromDiff (строка = блок; так же выглядит и будущий
// документ сырого markdown), форматы читаются наблюдателем
// ZDocument::highlightFormats. Что стережётся: заголовок крупнее на ступень,
// маркеры/номера/задачи/формулы акцентом, код на подложке в строке и между
// заборами (состояние сквозь строки), жирный/курсив, цены не формулы,
// убранная строка разности не подсвечивается — а состояние забора сквозь неё
// идёт.

#include "diff.h"
#include "document.h"
#include "settings.h"
#include "test_util.h"

#include <QTextLayout>

#include <functional>
#include <string>

namespace {

using zametti::ZDocument;
namespace diff = zametti::diff;

std::string n(long long v) { return std::to_string(v); }

// Документ, где каждая строка md — блок Same (как в сыром markdown). Строки
// берутся КАК ЕСТЬ, без канонизации: подсветчик обязан понимать и «*»-буллеты,
// и «_курсив_», и «-[x]» — то, что человек набирает, а не то, что запишет
// сериализатор.
ZDocument rawDoc(const char* md) {
    diff::Result result;
    int i = 0;
    for (const QString& line : QString::fromUtf8(md).split(QLatin1Char('\n'))) {
        diff::Row row;
        row.mark = diff::Mark::Same;
        row.before = row.after = i++;
        row.textBefore = row.textAfter = line;
        result.rows.append(row);
    }
    return ZDocument::fromDiff(result);
}

// Каждый знак отрезка [from, to) блока накрыт форматом с таким свойством
// (отрезок может быть разбит на несколько форматов: setFormat дробит).
bool hasProperty(const ZDocument& doc, int block, int from, int to,
                 const std::function<bool(const QTextCharFormat&)>& test) {
    if (from >= to) return false;
    const QList<QTextLayout::FormatRange> formats = doc.highlightFormats(block);
    for (int at = from; at < to; ++at) {
        bool covered = false;
        for (const QTextLayout::FormatRange& r : formats)
            if (r.start <= at && at < r.start + r.length && test(r.format)) covered = true;
        if (!covered) return false;
    }
    return true;
}

QStringList linesOfDoc(const ZDocument& doc) {
    QStringList out;
    for (int i = 0; i < doc.blockCount(); ++i) out.append(doc.blockAt(i).text);
    return out;
}

void checkRules() {
    const zametti::ZSettings::MarkdownHighlighting& rules = zametti::settings().markdownHighlighting();
    const QColor accent = rules.accent();
    const QColor code = rules.codeBackground();
    const int headingStep = zametti::settings().style().diffStep() + rules.headingStep();
    const auto isAccent = [&](const QTextCharFormat& f) { return f.foreground().color() == accent; };
    const auto isBoldAccent = [&](const QTextCharFormat& f) {
        return f.foreground().color() == accent && f.fontWeight() == QFont::Bold;
    };
    const auto isLink = [&](const QTextCharFormat& f) { return f.foreground().color() == rules.link(); };
    const auto isImage = [&](const QTextCharFormat& f) { return f.foreground().color() == rules.image(); };
    const auto isCode = [&](const QTextCharFormat& f) { return f.background().color() == code; };
    const auto isHeading = [&](const QTextCharFormat& f) {
        return f.hasProperty(QTextFormat::FontSizeAdjustment) &&
               f.intProperty(QTextFormat::FontSizeAdjustment) == headingStep;
    };
    const auto isBold = [](const QTextCharFormat& f) { return f.fontWeight() == QFont::Bold; };
    const auto isItalic = [](const QTextCharFormat& f) { return f.fontItalic(); };

    const ZDocument doc = rawDoc(
        "# Заголовок **жирный**\n"
        "\n"
        "- пункт\n"
        "* ещё **жирный** и _курсив_ и *тоже курсив*\n"
        "1. номер\n"
        "-[x] задача\n"
        "- [ ] другая\n"
        "код `внутри` строки\n"
        "формула $x^2$ и $$a+b$$ и цена $5 и $7\n"
        "```\n"
        "int main();\n"
        "```\n"
        "после забора\n"
        "снова_не_курсив и 2*3*4\n"
        "ссылка [сюда](https://x.y/z) и <https://a.b/> и ![снимок](img/1.jpg)\n");
    const QStringList lines = linesOfDoc(doc);
    const auto at = [&](const char* text) { return int(lines.indexOf(QString::fromUtf8(text))); };
    const int h = at("# Заголовок **жирный**");
    ZT_TRUE("строки на месте", h >= 0);
    ZT_TRUE("заголовок — вся строка на ступень крупнее (" + n(headingStep) + ")",
            hasProperty(doc, h, 2, int(lines[h].size()), isHeading));
    ZT_TRUE("и жирная", hasProperty(doc, h, 2, int(lines[h].size()), isBold));
    ZT_TRUE("маркер заголовка — акцентом и жирный", hasProperty(doc, h, 0, 1, isBoldAccent));
    ZT_TRUE("жирный внутри заголовка — жирный И крупный",
            hasProperty(doc, h, 12, 22, [&](const QTextCharFormat& f) { return isBold(f) && isHeading(f); }));

    const int bullet = at("- пункт");
    ZT_TRUE("буллет «- » акцентом и жирный", hasProperty(doc, bullet, 0, 1, isBoldAccent));
    ZT_TRUE("а текст пункта — нет", !hasProperty(doc, bullet, 2, 7, isAccent));
    const int star = at("* ещё **жирный** и _курсив_ и *тоже курсив*");
    ZT_TRUE("буллет «* » акцентом и жирный", hasProperty(doc, star, 0, 1, isBoldAccent));
    ZT_TRUE("**жирный** — жирный", hasProperty(doc, star, 6, 16, isBold));
    ZT_TRUE("_курсив_ — курсив", hasProperty(doc, star, 19, 27, isItalic));
    ZT_TRUE("*курсив* — курсив", hasProperty(doc, star, 30, 43, isItalic));
    ZT_TRUE("«1. » акцентом и жирный", hasProperty(doc, at("1. номер"), 0, 2, isBoldAccent));
    ZT_TRUE("«-[x] » акцентом и жирный", hasProperty(doc, at("-[x] задача"), 0, 4, isBoldAccent));
    ZT_TRUE("«- [ ] » акцентом и жирный", hasProperty(doc, at("- [ ] другая"), 0, 5, isBoldAccent));
    const int inl = at("код `внутри` строки");
    ZT_TRUE("код в строке — на подложке", hasProperty(doc, inl, 4, 12, isCode));
    ZT_TRUE("а слова вокруг — нет", !hasProperty(doc, inl, 0, 3, isCode));
    const int math = at("формула $x^2$ и $$a+b$$ и цена $5 и $7");
    ZT_TRUE("$x^2$ акцентом", hasProperty(doc, math, 8, 13, isAccent));
    ZT_TRUE("но не жирным", !hasProperty(doc, math, 8, 13, isBold));
    ZT_TRUE("$$a+b$$ акцентом", hasProperty(doc, math, 16, 23, isAccent));
    ZT_TRUE("цена «$5 и $7» — не формула", !hasProperty(doc, math, 31, 33, isAccent));
    // Между заборами подсветчик знаки НЕ красит (плашку кладёт вид, иначе фон
    // под знаками вдвое темнее — нашёл владелец), а состояние ставит.
    const int body = at("int main();");
    ZT_TRUE("код между заборами — без подложки на знаках", !hasProperty(doc, body, 0, 11, isCode));
    ZT_EQ("но в состоянии «в заборе»", n(1), n(doc.blockStateAt(body)));
    ZT_EQ("открывающий забор — «в заборе»", n(1), n(doc.blockStateAt(at("```"))));
    ZT_EQ("после забора — обычное состояние", n(0), n(doc.blockStateAt(at("после забора"))));
    const int under = at("снова_не_курсив и 2*3*4");
    ZT_TRUE("подчёркивания внутри слова — не курсив", !hasProperty(doc, under, 6, 8, isItalic));
    ZT_TRUE("звёздочки в арифметике — не курсив", !hasProperty(doc, under, 19, 22, isItalic));
    const int links = at("ссылка [сюда](https://x.y/z) и <https://a.b/> и ![снимок](img/1.jpg)");
    const auto underlined = [](const QTextCharFormat& f) { return f.fontUnderline(); };
    ZT_TRUE("[текст](адрес) — цветом ссылки", hasProperty(doc, links, 7, 28, isLink));
    ZT_TRUE("и с подчёркиванием", hasProperty(doc, links, 7, 28, underlined));
    ZT_TRUE("<адрес> — цветом ссылки", hasProperty(doc, links, 31, 45, isLink));
    ZT_TRUE("![подпись](файл) — цветом картинки", hasProperty(doc, links, 48, 68, isImage));
    ZT_TRUE("а не ссылки", !hasProperty(doc, links, 48, 68, isLink));
    ZT_TRUE("адрес картинки — с подчёркиванием", hasProperty(doc, links, 58, 67, underlined));
    ZT_TRUE("а подпись картинки — без", !hasProperty(doc, links, 50, 56, underlined));
}

void checkRemovedLinesSkipped() {
    // База с кодом внутри забора; в слепке средняя строка кода убрана.
    const QStringList base = diff::linesOf(std::string_view(
        "- пункт\n```\nодин\nдва\nтри\n```\nхвост\n"));
    const QStringList shown = diff::linesOf(std::string_view(
        "- пункт\n```\nодин\nтри\n```\nхвост\n"));
    const ZDocument doc = ZDocument::fromDiff(diff::compare(base, shown));
    const QStringList lines = linesOfDoc(doc);
    const int gone = int(lines.indexOf(QStringLiteral("два")));
    ZT_TRUE("убранная строка в документе есть", gone >= 0);
    ZT_EQ("и помечена убранной", n(int(diff::Mark::Removed)), n(doc.diffMarkAt(gone)));
    ZT_TRUE("убранная строка не подсвечивается", doc.highlightFormats(gone).isEmpty());
    const QColor code = zametti::settings().markdownHighlighting().codeBackground();
    const auto isCode = [&](const QTextCharFormat& f) { return f.background().color() == code; };
    const int after = int(lines.indexOf(QStringLiteral("три")));
    ZT_EQ("код ПОСЛЕ убранной строки — по-прежнему в заборе: состояние пронесено сквозь неё",
          n(1), n(doc.blockStateAt(after)));
    ZT_EQ("а хвост за забором — нет", n(0),
          n(doc.blockStateAt(int(lines.indexOf(QStringLiteral("хвост"))))));
    (void)isCode;
    // Повторный вызов подсветки — ничего не заводит второй раз (тот же ответ).
    ZDocument again = doc;
    again.highlightMarkdown(1);
    ZT_EQ("повторное прикрепление — те же форматы", n(doc.highlightFormats(after).size()),
          n(again.highlightFormats(after).size()));
}

}  // namespace

TEST(SyntaxHighlighter, All) {
    checkRules();
    checkRemovedLinesSkipped();
    EXPECT_EQ(0, zt::freshFailures());
}
