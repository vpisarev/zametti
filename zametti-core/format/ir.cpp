// Арена IR: построение, перенос между документами, проверка целостности.

#include "ir.h"

#include <cassert>
#include <cstdint>

namespace zametti {

Range Document::append(std::string_view bytes) {
    const int32_t at = int32_t(chars.size());
    if (!bytes.empty()) {
        // Кусок нашей же арены — законный вход: replace-by-append переписывает
        // текст блока его же байтами сплошь и рядом. Дописывать вид на самого
        // себя через append(const char*, n) нельзя (реаллокация оборвёт
        // источник на полуслове), поэтому такой случай отдаём форме
        // append(строка, откуда, сколько) — она самоналожение переживает.
        const char* base = chars.data();
        if (bytes.data() >= base && bytes.data() <= base + chars.size())
            chars.append(chars, size_t(bytes.data() - base), bytes.size());
        else
            chars.append(bytes);
    }
    return {at, int32_t(chars.size())};
}

Range Document::appendInlines(std::span<const Inline> items) {
    const int32_t at = int32_t(spans.size());
    spans.insert(spans.end(), items.begin(), items.end());
    return {at, int32_t(spans.size())};
}

Block Document::newBlock(Kind kind, std::string_view text) {
    Block b;
    b.kind = kind;
    b.text = append(text);
    return b;
}

Block Document::newRaw(std::string_view bytes) {
    Block b;
    // Род дословного куска — всегда Paragraph по умолчанию: у дословного рода
    // нет вовсе, а wouldMerge смотрит на kind, не спрашивая про raw.
    b.raw = true;
    b.text = append(bytes);
    if (b.text.empty() || chars[size_t(b.text.end) - 1] != '\n') {
        chars.push_back('\n');
        b.text.end = int32_t(chars.size());
    }
    return b;
}

Block Document::adopt(const Document& from, const Block& b) {
    // Свой же документ — законный случай (перекладывание блока внутри одной
    // арены). Тогда байты берутся из себя, и append это переживает.
    Block out = b;
    out.text = append(from.text(b));
    out.info = append(from.info(b));

    const int32_t first = b.inlines.start;
    const int32_t count = b.inlines.size() > 0 ? b.inlines.size() : 0;
    const int32_t base = int32_t(spans.size());
    // Резерв заранее: без него push_back переселил бы spans посреди обхода, а
    // мы читаем оттуда же (from может быть нами).
    spans.reserve(spans.size() + size_t(count));
    for (int32_t i = 0; i < count; ++i) {
        Inline s = from.spans[size_t(first + i)];   // текст спана относительный — не трогаем
        s.href = append(from.view(s.href));
        s.title = append(from.view(s.title));
        spans.push_back(s);
    }
    out.inlines = {base, base + count};
    return out;
}

bool Document::isClosedHtmlComment(const Block& b) const {
    if (!b.raw) return false;
    const std::string_view raw = text(b);
    if (raw.size() < 8) return false;
    return raw.compare(0, 4, "<!--") == 0 && raw.compare(raw.size() - 4, 4, "-->\n") == 0;
}

bool Document::wouldMerge(const Block& previous, const Block& next) const {
    // Правило живёт в block_kind.h — одно на разбор, на живой документ и на это
    // умирающее представление.
    return zametti::wouldMerge(previous.kind, previous.raw, isClosedHtmlComment(previous),
                               next.kind, next.raw);
}

void Document::validate() const {
#ifndef NDEBUG
    assert(chars.size() <= size_t(INT32_MAX) &&
           "заметка больше двух гигабайт в модель мира не входит");
    const int32_t charsSize = int32_t(chars.size());
    const int32_t spanCount = int32_t(spans.size());
    const auto insideArena = [&](Range r) {
        return r.start >= 0 && r.end >= r.start && r.end <= charsSize;
    };

    // Спаны между блоками не пересекаются: у каждого спана ровно один хозяин.
    std::vector<bool> owned(spans.size(), false);

    for (const Block& b : blocks) {
        assert(insideArena(b.text) && "текст блока вне арены");
        assert(insideArena(b.info) && "info блока вне арены");
        assert((!b.raw || b.kind == Kind::Paragraph) &&
               "у дословного куска рода нет: он обязан остаться Paragraph");
        assert((!b.raw || b.inlines.empty()) && "внутри дословного куска разметки не бывает");
        assert((!b.raw || b.info.empty()) && "у дословного куска info не бывает");
        // Формула — литеральный блок: разметки внутри не бывает по определению,
        // как и у блока кода.
        assert((b.kind != Kind::Math || b.inlines.empty()) &&
               "внутри блока-формулы разметки не бывает");
        assert(b.inlines.start >= 0 && b.inlines.end >= b.inlines.start &&
               b.inlines.end <= spanCount && "спаны блока вне spans");

        for (int32_t i = b.inlines.start; i < b.inlines.end; ++i) {
            assert(!owned[size_t(i)] && "спан принадлежит двум блокам сразу");
            owned[size_t(i)] = true;
            const Inline& s = spans[size_t(i)];
            // Второе координатное пространство: текст спана меряется от текста
            // блока, поэтому и границы у него блочные, а не арены.
            assert(s.text.start >= 0 && s.text.end >= s.text.start &&
                   s.text.end <= b.text.size() && "спан вне текста своего блока");
            assert(insideArena(s.href) && "href спана вне арены");
            assert(insideArena(s.title) && "title спана вне арены");
        }
    }
#endif
}

}  // namespace zametti
