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
    code_.setBackground(rules_.codeBackground());
    // Заголовки: НЕ абсолютный кегль, а ступень от шрифта документа — иначе
    // масштаб (setDefaultFont) их не тронул бы. Ступень прибавляется к ступени
    // строки: у строк разности она diffStep, у сырого markdown — 0.
    heading_.setProperty(QTextFormat::FontSizeAdjustment,
                         std::clamp(baseStep + rules_.headingStep(), kFontStepMin, kFontStepMax));
    bold_.setFontWeight(QFont::Bold);
    italic_.setFontItalic(true);

    fence_ = QRegularExpression(QStringLiteral("^\\s{0,3}(```|~~~)"));
    heading_re_ = QRegularExpression(QStringLiteral("^\\s{0,3}#{1,6}(\\s|$)"));
    // Задача: маркер, пробел (или без — краткая запись автозамены), скобки.
    task_ = QRegularExpression(QStringLiteral("^\\s*[-*+]\\s?\\[[ xX]\\](?=\\s|$)"));
    bullet_ = QRegularExpression(QStringLiteral("^\\s*[-*+](?=\\s)"));
    ordered_ = QRegularExpression(QStringLiteral("^\\s*\\d{1,9}[.)](?=\\s)"));
    codeSpan_ = QRegularExpression(QStringLiteral("`[^`\\n]+`"));
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
                                   &displayMath_, &inlineMath_, &boldStar_, &boldUnder_,
                                   &italicStar_, &italicUnder_})
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

    // Забор кода: строка забора и всё между заборами — на подложке целиком.
    const bool fenceLine = fence_.match(text).hasMatch();
    if (previous == InFence) {
        setFormat(0, int(text.size()), code_);
        setCurrentBlockState(fenceLine ? Plain : InFence);
        return;
    }
    if (fenceLine) {
        setFormat(0, int(text.size()), code_);
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
        setFormat(0, hashes, mark);
    }

    // Маркеры в начале строки: задача, буллет, номер — акцентом.
    for (const QRegularExpression* re : {&task_, &bullet_, &ordered_}) {
        const QRegularExpressionMatch m = re->match(text);
        if (!m.hasMatch()) continue;
        const int to = int(m.capturedEnd());
        setFormat(0, to, accent_);
        for (int i = 0; i < to; ++i) taken[i] = true;
        break;
    }

    // Атомарные спаны — код и формулы — первыми: внутри них ничего другого.
    applySpans(text, codeSpan_, code_, taken, base);
    applySpans(text, displayMath_, accent_, taken, base);
    applySpans(text, inlineMath_, accent_, taken, base);
    // Начертания: жирный раньше курсива (** внутри * иначе съел бы одну звезду).
    applySpans(text, boldStar_, bold_, taken, base);
    applySpans(text, boldUnder_, bold_, taken, base);
    applySpans(text, italicStar_, italic_, taken, base);
    applySpans(text, italicUnder_, italic_, taken, base);
}

}  // namespace zametti
