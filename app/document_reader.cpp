#include "document_reader.h"

#include "doc_model.h"

#include <QString>
#include <QTextBlock>
#include <QTextCharFormat>
#include <QTextDocument>
#include <QTextFragment>

namespace zametti {
namespace {

std::string toUtf8(const QString& text) {
    const QByteArray utf8 = text.toUtf8();
    return std::string(utf8.constData(), static_cast<size_t>(utf8.size()));
}

bool sameStyle(const Span& a, const Span& b) {
    return a.bold == b.bold && a.italic == b.italic && a.strike == b.strike &&
           a.code == b.code && a.href == b.href;
}

// Блок читается одним проходом по кускам: и текст, и спаны. Смещение копится в
// байтах — куски идут подряд и покрывают блок целиком, так что складывать длины
// достаточно, пересчитывать позиции не надо.
//
// Разделитель строк превращается обратно в исходный знак только там, где стоит
// пометка BreakSourceProperty. Без неё это чужой U+2028 из самого текста
// заметки, и трогать его нельзя.
void readBlock(const QTextBlock& block, Block& out, bool withSpans) {
    for (QTextBlock::iterator it = block.begin(); !it.atEnd(); ++it) {
        const QTextFragment fragment = it.fragment();
        if (!fragment.isValid()) continue;

        const QTextCharFormat format = fragment.charFormat();
        QString piece = fragment.text();
        switch (format.intProperty(BreakSourceProperty)) {
            case BreakNewline:
                piece.replace(QChar::LineSeparator, QLatin1Char('\n'));
                break;
            case BreakCarriageReturn:
                piece.replace(QChar::LineSeparator, QLatin1Char('\r'));
                break;
            case BreakParagraph:
                piece.replace(QChar::LineSeparator, QChar(QChar::ParagraphSeparator));
                break;
            default:
                break;   // чужой U+2028 из самого текста — трогать нельзя
        }

        const std::string text = toUtf8(piece);
        if (text.empty()) continue;

        const int offset = static_cast<int>(out.text.size());
        out.text += text;
        if (!withSpans) continue;

        const int style = format.intProperty(SpanStyleProperty);
        const QString href = format.anchorHref();
        if (style == 0 && href.isEmpty()) continue;

        Span span;
        span.offset = offset;
        span.length = static_cast<int>(text.size());
        span.bold = (style & SpanBold) != 0;
        span.italic = (style & SpanItalic) != 0;
        span.strike = (style & SpanStrike) != 0;
        span.code = (style & SpanCode) != 0;
        span.href = toUtf8(href);

        // Куски дробятся и без смены стиля: мягкий перенос помечен отдельно,
        // эмодзи набраны другим кеглем. Такие соседи склеиваются, иначе IR
        // разошёлся бы с разбором файла, где спан один.
        if (!out.inlines.empty() && sameStyle(out.inlines.back(), span) &&
            out.inlines.back().offset + out.inlines.back().length == span.offset) {
            out.inlines.back().length += span.length;
        } else {
            out.inlines.push_back(std::move(span));
        }
    }
}

// Пустой документ Qt не бывает: один блок в нём есть всегда. Свойств у этого
// блока нет — их ставит сборщик, а ему нечего было ставить. Отличить его от
// настоящего пустого абзаца больше нечем, поэтому правило узкое: единственный
// блок, пустой и без свойств.
bool isPhantomBlock(const QTextDocument& doc, const QTextBlock& block) {
    if (doc.blockCount() != 1 || !block.text().isEmpty()) return false;
    const QTextBlockFormat format = block.blockFormat();
    return !format.hasProperty(KindProperty) && !format.hasProperty(RawProperty);
}

}  // namespace

Document readDocument(const QTextDocument& doc) {
    Document out;

    // Литеральные блоки лежат в документе построчно, по QTextBlock на строку, и
    // склеиваются здесь. Признак продолжения обязателен: без него разрезанный
    // блок кода из двух строк не отличить от двух блоков кода подряд, а это
    // разный markdown.
    Block pending;
    bool hasPending = false;
    bool pendingRaw = false;

    auto flush = [&] {
        if (!hasPending) return;
        if (pendingRaw) {
            pending.rawSource = std::move(pending.text);
            pending.text.clear();
        }
        out.push_back(std::move(pending));
        pending = Block{};
        hasPending = false;
    };

    for (QTextBlock block = doc.begin(); block.isValid(); block = block.next()) {
        if (isPhantomBlock(doc, block)) continue;

        const QTextBlockFormat format = block.blockFormat();
        const bool raw = isRawBlock(block);

        if (isContinuationBlock(block) && hasPending) {
            pending.text.push_back('\n');
            readBlock(block, pending, false);
        } else {
            flush();
            hasPending = true;
            pendingRaw = raw;
            // Плотность стыка — свойство границы, и живёт она на первом блоке
            // логического: строки-продолжения к своему соседу и так вплотную.
            pending.tight = isTightBlock(block);
            if (!raw) {
                pending.kind = kindOf(block);
                if (pending.kind == Kind::Heading) pending.headingLevel = format.headingLevel();
                if (isList(pending.kind)) pending.level = levelOf(block);
                if (pending.kind == Kind::Code)
                    pending.info = toUtf8(format.stringProperty(InfoProperty));
            }
            readBlock(block, pending, !raw);
        }

        // Признак стоит на последней строке блока — там, где перевод и был.
        if (format.boolProperty(TrailingNewlineProperty)) pending.text.push_back('\n');
    }
    flush();
    return out;
}

}  // namespace zametti
