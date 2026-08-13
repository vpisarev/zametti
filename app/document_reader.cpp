#include "document_reader.h"

#include "math_scan.h"

#include "doc_model.h"

#include <QString>
#include <QTextBlock>
#include <QTextCharFormat>
#include <QTextDocument>
#include <QTextFragment>

namespace zametti {
namespace {
// Является ли этот текст ОДНОЙ формулой целиком. Спрашивается общий канон, а не
// «начинается с доллара»: иначе вид и разбор разошлись бы на первом же краю.
bool wholeMath(const std::string& text) {
    const std::vector<MathSpan> found = scanMath(text);
    return found.size() == 1 && found.front().start == 0 &&
           size_t(found.front().end) == text.size();
}



std::string toUtf8(const QString& text) {
    const QByteArray utf8 = text.toUtf8();
    return std::string(utf8.constData(), static_cast<size_t>(utf8.size()));
}

bool sameStyle(const Document& doc, const Inline& a, const Inline& b) {
    return a.flags == b.flags && doc.view(a.href) == doc.view(b.href) &&
           doc.view(a.title) == doc.view(b.title);
}

// Блок читается одним проходом по кускам: и текст, и спаны. Смещение копится в
// байтах — куски идут подряд и покрывают блок целиком, так что складывать длины
// достаточно, пересчитывать позиции не надо. Смещение спана относительное, от
// начала текста блока, — ровно то, что здесь и копится.
//
// Текст блока собирается в буфере text и уедет в арену одним куском при
// закрытии блока; спаны пишутся сразу в doc.spans, спаны текущего блока —
// хвост от spanStart.
//
// Разделитель строк превращается обратно в исходный знак только там, где стоит
// пометка BreakSourceProperty. Без неё это чужой U+2028 из самого текста
// заметки, и трогать его нельзя.
void readBlock(const QTextBlock& block, Document& doc, std::string& text, int32_t spanStart,
               bool withSpans) {
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

        const std::string bytes = toUtf8(piece);
        if (bytes.empty()) continue;

        const int32_t offset = static_cast<int32_t>(text.size());
        text += bytes;
        if (!withSpans) continue;

        const int style = format.intProperty(SpanStyleProperty);
        const QString href = format.anchorHref();
        if (style == 0 && href.isEmpty()) continue;

        Inline span;
        span.text = {offset, offset + static_cast<int32_t>(bytes.size())};
        span.set(InlineBold, (style & SpanBold) != 0);
        span.set(InlineItalic, (style & SpanItalic) != 0);
        span.set(InlineStrike, (style & SpanStrike) != 0);
        span.set(InlineCode, (style & SpanCode) != 0);
        span.href = doc.append(toUtf8(href));

        // Подпись картинки плоская по построению (см. разбор): правки могли
        // домешать в формат другие биты — они здесь гасятся, иначе IR выразит
        // то, что файл выразить не может. Картинка без пути — не картинка.
        span.set(InlineImage, (style & SpanImage) != 0 && !span.href.empty());
        if (span.image()) {
            span.flags = InlineImage;
            span.title = doc.append(toUtf8(format.property(SpanTitleProperty).toString()));
        }

        // Строчный комментарий плоский так же; внутренность с "-->" файл
        // выразить не может — такой спан перестаёт быть комментарием и
        // становится видимым текстом (сериализатор его экранирует).
        span.set(InlineComment, (style & SpanComment) != 0 && !span.image() &&
                                    bytes.find("-->") == std::string::npos);
        if (span.comment()) {
            span.flags = InlineComment;
            span.href = Range{};
            span.title = Range{};
        }

        // ФОРМУЛА ОБЯЗАНА ПЕРЕЖИТЬ КРУГ «ДОКУМЕНТ → IR». Не переживёт — файл
        // испортится молча при первой же записи: сериализатор экранирует
        // доллар, который начал бы формулу, и `$x^2$` ушло бы на диск как
        // `\$x^2$`. Поэтому спрашиваем не только бит стиля, но и сам текст:
        // правка могла разорвать формулу пополам, и тогда это уже не формула, а
        // текст с долларами (канон — общий, из ядра).
        span.set(InlineMath, (style & SpanMath) != 0 && !span.image() && !span.comment() &&
                                 wholeMath(bytes));
        if (span.math()) {
            // Плоская, как картинка и комментарий: внутри формулы разметки нет.
            span.flags = InlineMath;
            span.href = Range{};
            span.title = Range{};
        }

        // Куски дробятся и без смены стиля: мягкий перенос помечен отдельно,
        // эмодзи набраны другим кеглем. Такие соседи склеиваются, иначе IR
        // разошёлся бы с разбором файла, где спан один.
        const bool haveOwn = doc.spans.size() > static_cast<size_t>(spanStart);
        if (haveOwn && sameStyle(doc, doc.spans.back(), span) &&
            doc.spans.back().text.end == span.text.start) {
            doc.spans.back().text.end = span.text.end;
        } else {
            doc.spans.push_back(span);
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
    // Метаданных в QTextDocument нет и не бывает — редактор их не видит.
    // Прицепить их к прочитанному — забота сохранения (см. document_saver).
    Document result;

    // Литеральные блоки лежат в документе построчно, по QTextBlock на строку, и
    // склеиваются здесь. Признак продолжения обязателен: без него разрезанный
    // блок кода из двух строк не отличить от двух блоков кода подряд.
    Block pending;
    std::string text;
    int32_t spanStart = 0;
    bool hasPending = false;
    bool pendingRaw = false;

    auto flush = [&] {
        if (!hasPending) return;
        // У дословного куска рода нет: он остаётся Paragraph, а текст блока и
        // есть его дословные байты.
        if (pendingRaw) {
            pending.raw = true;
            pending.kind = Kind::Paragraph;
            pending.info = Range{};
        }
        // Комментарий держит свой инвариант на границе документ→IR: разметки
        // внутри не бывает (набранные поверх биты — мусор правок), а
        // внутренность с "-->" файл выразить не может — такой блок перестаёт
        // быть комментарием и становится видимым текстом.
        if (!pending.raw && pending.kind == Kind::Html) {
            if (text.find("-->") != std::string::npos)
                pending.kind = Kind::Paragraph;
            else
                result.spans.resize(static_cast<size_t>(spanStart));
        }
        pending.text = result.append(text);
        pending.inlines = {spanStart, static_cast<int32_t>(result.spans.size())};
        result.blocks.push_back(pending);

        pending = Block{};
        text.clear();
        spanStart = static_cast<int32_t>(result.spans.size());
        hasPending = false;
    };

    for (QTextBlock block = doc.begin(); block.isValid(); block = block.next()) {
        if (isPhantomBlock(doc, block)) continue;

        const QTextBlockFormat format = block.blockFormat();
        const bool raw = isRawBlock(block);

        if (isContinuationBlock(block) && hasPending) {
            text.push_back('\n');
            readBlock(block, result, text, spanStart, false);
        } else {
            flush();
            hasPending = true;
            pendingRaw = raw;
            if (!raw) {
                pending.kind = kindOf(block);
                if (pending.kind == Kind::Heading)
                    pending.headingLevel = static_cast<int8_t>(format.headingLevel());
                if (isList(pending.kind)) {
                    pending.marker = markerOf(block).marker;
                    pending.checked = markerOf(block).checked;
                }
                pending.level = static_cast<int16_t>(levelOf(block));
                if (pending.kind == Kind::Code)
                    pending.info = result.append(toUtf8(format.stringProperty(InfoProperty)));
            }
            // Разметку внутри блока кода не читаем: содержимое там буквальное,
            // и сборщик её всё равно не поставит — прочитанное разошлось бы с
            // собранным, а на этом стоит инвариант A.
            readBlock(block, result, text, spanStart, !raw && pending.kind != Kind::Code);
        }

        // Признак стоит на последней строке блока — там, где перевод и был.
        if (format.boolProperty(TrailingNewlineProperty)) text.push_back('\n');
    }
    flush();
    result.validate();
    return result;
}

}  // namespace zametti
