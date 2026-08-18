#include "syntax_highlighter.h"

#include "diff.h"
#include "doc_model.h"

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
    // Заголовки: НЕ абсолютный кегль, а ступень от шрифта документа — иначе
    // масштаб (setDefaultFont) их не тронул бы. Ступень прибавляется к ступени
    // строки: у строк разности она diffStep, у сырого markdown — 0.
    heading_.setProperty(QTextFormat::FontSizeAdjustment,
                         std::clamp(baseStep + rules_.headingStep(), kFontStepMin, kFontStepMax));
    heading_.setFontWeight(QFont::Bold);
    bold_.setFontWeight(QFont::Bold);
    italic_.setFontItalic(true);

    fence_ = QRegularExpression(QStringLiteral("^\\s{0,3}(```|~~~)"));
    heading_re_ = QRegularExpression(QStringLiteral("^\\s{0,3}#{1,6}(\\s|$)"));
    // Задача: маркер, пробел (или без — краткая запись автозамены), скобки.
    task_ = QRegularExpression(QStringLiteral("^\\s*[-*+]\\s?\\[[ xX]\\](?=\\s|$)"));
    bullet_ = QRegularExpression(QStringLiteral("^\\s*[-*+](?=\\s)"));
    ordered_ = QRegularExpression(QStringLiteral("^\\s*\\d{1,9}[.)](?=\\s)"));
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
    for (QRegularExpression* re : {&fence_, &heading_re_, &task_, &bullet_, &ordered_, &codeSpan_,
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
    const int previous = previousBlockState() < 0 ? Plain : previousBlockState();

    // Убранная строка разности — не часть слепка: не подсвечиваем, состояние
    // забора проносим сквозь неё.
    if (diffMarkOf(currentBlock()) == int(diff::Mark::Removed)) {
        setCurrentBlockState(previous);
        return;
    }

    // Забор кода: строка забора и всё между заборами — только состояние; плашку
    // во всю колонку кладёт вид (красить знаки ещё и здесь — фон под ними был
    // бы вдвое темнее плашки, так и вышло в первой примерке).
    const bool fenceLine = fence_.match(text).hasMatch();
    if (previous == InFence) {
        setCurrentBlockState(fenceLine ? Plain : InFence);
        return;
    }
    if (fenceLine) {
        setCurrentBlockState(InFence);
        return;
    }
    setCurrentBlockState(Plain);
    if (text.isEmpty()) return;

    QVector<bool> taken(int(text.size()), false);
    QTextCharFormat base;   // формат строки, поверх которого ложатся спаны

    // Заголовок — вся строка крупнее; маркер `#` — акцентом.
    if (const QRegularExpressionMatch h = heading_re_.match(text); h.hasMatch()) {
        base = heading_;
        setFormat(0, int(text.size()), heading_);
        int hashes = 0;
        while (hashes < text.size() && (text[hashes] == QLatin1Char(' ') || text[hashes] == QLatin1Char('#')))
            ++hashes;
        QTextCharFormat mark = heading_;
        mark.setForeground(accent_.foreground());
        setFormat(0, hashes, mark);   // жирный и крупный уже в heading_
    }

    // Маркеры в начале строки: задача, буллет, номер — акцентом.
    for (const QRegularExpression* re : {&task_, &bullet_, &ordered_}) {
        const QRegularExpressionMatch m = re->match(text);
        if (!m.hasMatch()) continue;
        const int to = int(m.capturedEnd());
        setFormat(0, to, marker_);
        for (int i = 0; i < to; ++i) taken[i] = true;
        break;
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
