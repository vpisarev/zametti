#include "syntax_highlighter.h"

#include "diff.h"
#include "doc_model.h"
#include "list_line.h"

#include <QTextBlock>
#include <QTextDocument>

namespace zametti {

ZSyntaxHighlighterMD::ZSyntaxHighlighterMD(QTextDocument* document,
                                           ZSettings::MarkdownHighlighting rules, int baseStep)
    : QSyntaxHighlighter(document), rules_(std::move(rules)) {
    accent_.setForeground(rules_.accent());
    marker_ = accent_;
    marker_.setFontWeight(QFont::Bold);
    code_.setBackground(rules_.codeBackground());
    link_.setForeground(rules_.link());
    link_.setFontUnderline(true);   // ссылки — с подчёркиванием (просьба владельца)
    image_.setForeground(rules_.image());
    comment_.setForeground(rules_.comment());
    // Заголовки: НЕ абсолютный кегль, а ступень от шрифта документа — иначе
    // масштаб (setDefaultFont) их не тронул бы. Ступень прибавляется к ступени
    // строки: у строк разности она diffStep, у сырого markdown — 0.
    heading_.setProperty(QTextFormat::FontSizeAdjustment,
                         std::clamp(baseStep + rules_.headingStep(), kFontStepMin, kFontStepMax));
    heading_.setFontWeight(QFont::Bold);
    // Глубокие заголовки: размер не трогаем вовсе (строка остаётся своего
    // кегля — у строк разности diffStep), только начертание.
    headingSmall_.setFontWeight(QFont::Bold);
    headingSmall_.setFontItalic(true);
    bold_.setFontWeight(QFont::Bold);
    italic_.setFontItalic(true);

    // Забор с любым отступом (внутри пункта списка он сдвинут); колонка забора
    // уходит в состояние блока.
    fence_ = QRegularExpression(QStringLiteral("^(\\s*)(```|~~~)"));
    heading_re_ = QRegularExpression(QStringLiteral("^\\s{0,3}#{1,6}(\\s|$)"));
    codeSpan_ = QRegularExpression(QStringLiteral("`[^`\\n]+`"));
    // Картинка раньше ссылки: `![…](…)` содержит `[…](…)`.
    imageLink_ = QRegularExpression(QStringLiteral("!\\[[^\\]\\n]*\\]\\(([^)\\n]*)\\)"));
    link_re_ = QRegularExpression(QStringLiteral("\\[[^\\]\\n]+\\]\\([^)\\n]*\\)"));
    autoLink_ = QRegularExpression(QStringLiteral("<(https?|ftp|mailto):[^>\\s]+>"));
    // Голый адрес (автоссылка GFM): до пробела или скобки; хвостовые
    // `.,;:!?` — не адрес, а конец фразы (владелец: ссылка на ozon в заметке
    // «Пробуем Obsidian» — без скобок).
    bareUrl_ = QRegularExpression(
        QStringLiteral("(?<![\\w/@.])(https?://|www\\.)[^\\s<>()\\[\\]]*[^\\s<>()\\[\\].,;:!?]"));
    displayMath_ = QRegularExpression(QStringLiteral("\\$\\$[^$]+?\\$\\$"));
    // Одиночный $ по канону pandoc: открывающему — непробел справа,
    // закрывающему — непробел слева и не цифра справа (см. правку md4c в
    // сессии 6); цена «$5 и $7» формулой не считается.
    inlineMath_ = QRegularExpression(QStringLiteral("(?<!\\$)\\$(?=\\S)[^$\\n]*?(?<=\\S)\\$(?!\\d|\\$)"));
    // \\w у QRegularExpression по умолчанию — только ASCII: без
    // UseUnicodeProperties «снова_не_курсив» стал бы курсивом.
    boldStar_ = QRegularExpression(QStringLiteral("\\*\\*(?=\\S)[^*\\n]+?(?<=\\S)\\*\\*"));
    boldUnder_ = QRegularExpression(QStringLiteral("(?<![\\w_])__(?=\\S)[^_\\n]+?(?<=\\S)__(?![\\w_])"));
    italicStar_ = QRegularExpression(QStringLiteral("(?<![\\w*])\\*(?=[^\\s*])[^*\\n]+?(?<=[^\\s*])\\*(?![\\w*])"));
    italicUnder_ = QRegularExpression(QStringLiteral("(?<![\\w_])_(?=[^\\s_])[^_\\n]+?(?<=[^\\s_])_(?![\\w_])"));
    for (QRegularExpression* re : {&fence_, &heading_re_, &codeSpan_,
                                   &imageLink_, &link_re_, &autoLink_, &bareUrl_, &displayMath_,
                                   &inlineMath_,
                                   &boldStar_, &boldUnder_, &italicStar_, &italicUnder_})
        re->setPatternOptions(QRegularExpression::UseUnicodePropertiesOption);
}

void ZSyntaxHighlighterMD::applySpans(const QString& text, const QRegularExpression& re,
                                       const QTextCharFormat& format, QVector<bool>& taken,
                                       const QTextCharFormat& base) {
    // setFormat ЗАМЕНЯЕТ формат отрезка, а не сливает: спан внутри заголовка
    // обязан остаться крупным — сливаем сами поверх формата строки.
    QTextCharFormat merged = base;
    merged.merge(format);
    QRegularExpressionMatchIterator it = re.globalMatch(text);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        const int from = int(m.capturedStart());
        const int to = int(m.capturedEnd());
        bool free = true;
        for (int i = from; i < to && free; ++i) free = !taken[i];
        if (!free) continue;
        setFormat(from, to - from, merged);
        for (int i = from; i < to; ++i) taken[i] = true;
    }
}

void ZSyntaxHighlighterMD::highlightBlock(const QString& text) {
    // -1 у Qt значит «состояния нет» — это Plain; InComment (-2) — своё.
    const int previous = previousBlockState() == -1 ? int(Plain) : previousBlockState();

    // Убранная строка разности — не часть слепка: не подсвечиваем, состояние
    // забора проносим сквозь неё.
    if (diffMarkOf(currentBlock()) == int(diff::Mark::Removed)) {
        setCurrentBlockState(previous);
        return;
    }

    // Забор кода: строка забора и всё между заборами — только состояние; плашку
    // во всю колонку кладёт вид (красить знаки ещё и здесь — фон под ними был
    // бы вдвое темнее плашки, так и вышло в первой примерке).
    const QRegularExpressionMatch fence = fence_.match(text);
    const bool fenceLine = fence.hasMatch();
    if (inFence(previous)) {
        setCurrentBlockState(fenceLine ? Plain : previous);
        return;
    }
    if (fenceLine) {
        setCurrentBlockState(fenceState(int(fence.capturedLength(1))));
        return;
    }
    setCurrentBlockState(Plain);
    if (text.isEmpty()) {
        if (inComment(previous)) setCurrentBlockState(InComment);
        return;
    }

    QVector<bool> taken(int(text.size()), false);
    QTextCharFormat base;   // формат строки, поверх которого ложатся спаны

    // HTML-КОММЕНТАРИИ — прежде всего остального: внутри них ничего не
    // подсвечивается, а незакрытый тянется на следующие строки состоянием.
    {
        const QString open = QStringLiteral("<!--");
        const QString close = QStringLiteral("-->");
        int at = 0;
        bool inside = inComment(previous);
        while (at < text.size()) {
            if (!inside) {
                const int start = int(text.indexOf(open, at));
                if (start < 0) break;
                at = start;
                inside = true;
            }
            const int end = int(text.indexOf(close, at));
            const int to = end < 0 ? int(text.size()) : end + int(close.size());
            setFormat(at, to - at, comment_);
            for (int i = at; i < to; ++i) taken[i] = true;
            if (end < 0) {
                setCurrentBlockState(InComment);
                return;
            }
            at = to;
            inside = false;
        }
        // Строка целиком в комментарии — маркеров и заголовков в ней нет.
        if (taken[0]) {
            bool whole = true;
            for (bool t : taken) whole = whole && t;
            if (whole) return;
        }
    }

    // Заголовок — вся строка крупнее; маркер `#` — акцентом. (Строка, начатая
    // комментарием, заголовком не считается.)
    if (const QRegularExpressionMatch h = heading_re_.match(text); h.hasMatch() && !taken[0]) {
        int hashes = 0;
        int level = 0;
        while (hashes < text.size() && (text[hashes] == QLatin1Char(' ') || text[hashes] == QLatin1Char('#'))) {
            if (text[hashes] == QLatin1Char('#')) ++level;
            ++hashes;
        }
        // Верхние уровни — крупнее; глубже largeHeadingLevels — кеглем текста,
        // жирным курсивом.
        base = level <= rules_.largeHeadingLevels() ? heading_ : headingSmall_;
        setFormat(0, int(text.size()), base);
        QTextCharFormat mark = base;
        mark.setForeground(accent_.foreground());
        setFormat(0, hashes, mark);   // начертание уже в base
    }

    // Маркер пункта в начале строки (задача, буллет, номер) — акцентом. Что
    // считается маркером, решает parseListLine — одно правило на подсветку и на
    // клавиши режима исходника.
    if (const ListLine item = parseListLine(text); item.item && !taken[0]) {
        const int to = item.markerEnd;
        setFormat(0, to, marker_);
        for (int i = 0; i < to; ++i) taken[i] = true;
    }

    // Атомарные спаны — код и формулы — первыми: внутри них ничего другого.
    applySpans(text, codeSpan_, code_, taken, base);
    // Картинка: вся — цветом картинки, адрес в скобках — ещё и с подчёркиванием.
    {
        QTextCharFormat whole = base;
        whole.merge(image_);
        QTextCharFormat address = whole;
        address.setFontUnderline(true);
        QRegularExpressionMatchIterator it = imageLink_.globalMatch(text);
        while (it.hasNext()) {
            const QRegularExpressionMatch m = it.next();
            const int from = int(m.capturedStart());
            const int to = int(m.capturedEnd());
            bool free = true;
            for (int i = from; i < to && free; ++i) free = !taken[i];
            if (!free) continue;
            setFormat(from, to - from, whole);
            if (m.capturedLength(1) > 0)
                setFormat(int(m.capturedStart(1)), int(m.capturedLength(1)), address);
            for (int i = from; i < to; ++i) taken[i] = true;
        }
    }
    applySpans(text, link_re_, link_, taken, base);
    applySpans(text, autoLink_, link_, taken, base);
    applySpans(text, bareUrl_, link_, taken, base);
    applySpans(text, displayMath_, accent_, taken, base);
    applySpans(text, inlineMath_, accent_, taken, base);
    // Начертания: жирный раньше курсива (** внутри * иначе съел бы одну звезду).
    applySpans(text, boldStar_, bold_, taken, base);
    applySpans(text, boldUnder_, bold_, taken, base);
    applySpans(text, italicStar_, italic_, taken, base);
    applySpans(text, italicUnder_, italic_, taken, base);
}

}  // namespace zametti
