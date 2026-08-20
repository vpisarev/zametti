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
#include "syntax_highlighter.h"
#include "settings_hook.h"
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
        "ссылка [сюда](https://x.y/z) и <https://a.b/> и ![снимок](img/1.jpg)\n"
        "голая: https://www.ozon.ru/product/x-1, и www.site.org.\n");
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
    ZT_TRUE("но в состоянии «в заборе»", zametti::ZSyntaxHighlighterMD::inFence(doc.blockStateAt(body)));
    ZT_TRUE("открывающий забор — «в заборе»",
            zametti::ZSyntaxHighlighterMD::inFence(doc.blockStateAt(at("```"))));
    ZT_EQ("колонка забора без отступа — 0", n(0),
          n(zametti::ZSyntaxHighlighterMD::fenceColumn(doc.blockStateAt(body))));
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
    const int bare = at("голая: https://www.ozon.ru/product/x-1, и www.site.org.");
    ZT_TRUE("голый https-адрес — ссылка", hasProperty(doc, bare, 7, 38, isLink));
    ZT_TRUE("запятая после него — нет", !hasProperty(doc, bare, 38, 39, isLink));
    ZT_TRUE("www.-адрес — ссылка", hasProperty(doc, bare, 42, 54, isLink));
    ZT_TRUE("точка в конце фразы — не адрес", !hasProperty(doc, bare, 54, 55, isLink));
    // Строка владельца дословно («Пробуем Obsidian», ссылка на ozon без скобок).
    const ZDocument owner = rawDoc(
        "ссылка на тельняху: https://www.ozon.ru/product/longsliv-telnyashka-beregite-ptits-telnyashka-muzhskaya-858891201\n");
    const int len = int(linesOfDoc(owner)[0].size());
    ZT_TRUE("ссылка на ozon из заметки владельца — подсвечена целиком",
            hasProperty(owner, 0, 20, len, isLink));
    ZT_TRUE("а слова перед ней — нет", !hasProperty(owner, 0, 0, 6, isLink));
}

// Глубокие заголовки («###» и ниже по умолчанию) — кеглем текста, жирным
// курсивом; верхние — крупнее. Порог — настройка, и проверяется ДЕЙСТВИЕМ:
// подняли до 3 — «###» стал крупным.
void checkDeepHeadings() {
    const auto isItalic = [](const QTextCharFormat& f) { return f.fontItalic(); };
    const auto isBold = [](const QTextCharFormat& f) { return f.fontWeight() == QFont::Bold; };
    const auto hasSize = [](const QTextCharFormat& f) {
        return f.hasProperty(QTextFormat::FontSizeAdjustment);
    };
    const char* md = "# один\n## два\n### три\n#### четыре\n###### шесть\n";
    {
        ZT_EQ("по умолчанию крупных уровней два", n(2),
              n(zametti::settings().markdownHighlighting().largeHeadingLevels()));
        const ZDocument doc = rawDoc(md);
        ZT_TRUE("«#» крупный", hasProperty(doc, 0, 2, 6, hasSize));
        ZT_TRUE("«#» не курсив", !hasProperty(doc, 0, 2, 6, isItalic));
        ZT_TRUE("«##» крупный", hasProperty(doc, 1, 3, 6, hasSize));
        ZT_TRUE("«###» кеглем текста", !hasProperty(doc, 2, 4, 7, hasSize));
        ZT_TRUE("«###» жирный", hasProperty(doc, 2, 4, 7, isBold));
        ZT_TRUE("«###» курсивом", hasProperty(doc, 2, 4, 7, isItalic));
        ZT_TRUE("маркер «###» тоже курсивом и жирным",
                hasProperty(doc, 2, 0, 3, [&](const QTextCharFormat& f) { return isBold(f) && isItalic(f); }));
        ZT_TRUE("«####» кеглем текста, курсивом", !hasProperty(doc, 3, 5, 11, hasSize) &&
                                                      hasProperty(doc, 3, 5, 11, isItalic));
        ZT_TRUE("«######» кеглем текста, курсивом", !hasProperty(doc, 4, 7, 12, hasSize) &&
                                                        hasProperty(doc, 4, 7, 12, isItalic));
    }
    // Порог действует: три крупных уровня — «###» крупный и прямой, «####» нет.
    zametti::mutableSettingsForTests().markdownHighlighting().setLargeHeadingLevels(3);
    {
        const ZDocument doc = rawDoc(md);
        ZT_TRUE("с порогом 3 «###» крупный", hasProperty(doc, 2, 4, 7, hasSize));
        ZT_TRUE("и прямой", !hasProperty(doc, 2, 4, 7, isItalic));
        ZT_TRUE("а «####» по-прежнему курсивом", hasProperty(doc, 3, 5, 11, isItalic));
    }
    zametti::mutableSettingsForTests().markdownHighlighting().setLargeHeadingLevels(2);
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
    ZT_TRUE("код ПОСЛЕ убранной строки — по-прежнему в заборе: состояние пронесено сквозь неё",
            zametti::ZSyntaxHighlighterMD::inFence(doc.blockStateAt(after)));
    ZT_EQ("а хвост за забором — нет", n(0),
          n(doc.blockStateAt(int(lines.indexOf(QStringLiteral("хвост"))))));
    (void)isCode;
    // Повторный вызов подсветки — ничего не заводит второй раз (тот же ответ).
    ZDocument again = doc;
    again.highlightMarkdown(1);
    ZT_EQ("повторное прикрепление — те же форматы", n(doc.highlightFormats(after).size()),
          n(again.highlightFormats(after).size()));
}

// Забор с отступом (блок кода внутри пункта списка): состояние несёт колонку
// забора — по ней вид кладёт плашку с того же отступа.
void checkIndentedFence() {
    const ZDocument doc = rawDoc(
        "- пункт\n"
        "  ```\n"
        "  int a;\n"
        "\n"
        "  ```\n"
        "хвост\n");
    const QStringList lines = linesOfDoc(doc);
    const int open = int(lines.indexOf(QStringLiteral("  ```")));
    ZT_TRUE("забор с отступом опознан", zametti::ZSyntaxHighlighterMD::inFence(doc.blockStateAt(open)));
    ZT_EQ("и колонка забора — 2", n(2),
          n(zametti::ZSyntaxHighlighterMD::fenceColumn(doc.blockStateAt(open))));
    const int body = int(lines.indexOf(QStringLiteral("  int a;")));
    ZT_EQ("тело — та же колонка", n(2),
          n(zametti::ZSyntaxHighlighterMD::fenceColumn(doc.blockStateAt(body))));
    ZT_EQ("пустая строка внутри — тоже", n(2),
          n(zametti::ZSyntaxHighlighterMD::fenceColumn(doc.blockStateAt(body + 1))));
    ZT_EQ("после закрывающего забора — обычная строка", n(0),
          n(doc.blockStateAt(int(lines.indexOf(QStringLiteral("хвост"))))));
    // Забор с отступом больше трёх пробелов (вложенный список) — тоже забор.
    const ZDocument deep = rawDoc("    ```\n    x\n    ```\n");
    ZT_EQ("колонка 4", n(4), n(zametti::ZSyntaxHighlighterMD::fenceColumn(deep.blockStateAt(1))));
}

// HTML-комментарии серые, многострочные — состоянием; внутри них маркеры и
// формулы не подсвечиваются.
void checkComments() {
    const QColor grey = zametti::settings().markdownHighlighting().comment();
    const auto isComment = [&](const QTextCharFormat& f) { return f.foreground().color() == grey; };
    const ZDocument doc = rawDoc(
        "текст <!-- скрыто --> и дальше\n"
        "<!-- начало\n"
        "- не пункт $x$\n"
        "конец --> хвост **жирный**\n"
        "- пункт\n");
    const QStringList lines = linesOfDoc(doc);
    ZT_TRUE("комментарий в строке — серый", hasProperty(doc, 0, 6, 21, isComment));
    ZT_TRUE("а текст вокруг — нет", !hasProperty(doc, 0, 0, 5, isComment) && !hasProperty(doc, 0, 22, 30, isComment));
    ZT_TRUE("незакрытый — серый до конца строки", hasProperty(doc, 1, 0, int(lines[1].size()), isComment));
    ZT_TRUE("и состояние «в комментарии»", zametti::ZSyntaxHighlighterMD::inComment(doc.blockStateAt(1)));
    ZT_TRUE("строка внутри — серая целиком", hasProperty(doc, 2, 0, int(lines[2].size()), isComment));
    const QColor accent = zametti::settings().markdownHighlighting().accent();
    ZT_TRUE("маркер внутри комментария — не акцент",
            !hasProperty(doc, 2, 0, 1, [&](const QTextCharFormat& f) { return f.foreground().color() == accent; }));
    ZT_TRUE("закрывающая строка: до --> серая", hasProperty(doc, 3, 0, 9, isComment));
    ZT_TRUE("после --> — обычная подсветка (жирный)",
            hasProperty(doc, 3, 16, 26, [](const QTextCharFormat& f) { return f.fontWeight() == QFont::Bold; }));
    ZT_EQ("после закрытия — обычное состояние", n(0), n(doc.blockStateAt(3)));
    ZT_TRUE("следующий пункт — снова маркер",
            hasProperty(doc, 4, 0, 1, [&](const QTextCharFormat& f) { return f.foreground().color() == accent; }));
}

}  // namespace

TEST(SyntaxHighlighter, All) {
    checkRules();
    checkDeepHeadings();
    checkRemovedLinesSkipped();
    checkIndentedFence();
    checkComments();
    EXPECT_EQ(0, zt::freshFailures());
}
