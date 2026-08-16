// markdown → ZDocument. Разбор md4c, идущий прямо в живой документ.
//
// Промежуточного представления нет: то, что раньше было ареной заметки, здесь
// стало ЧЕРНОВИКОМ РАЗБОРА — местным буфером на один вызов. Разница не в
// словах: черновик не переживает разбор, наружу не выходит и второй живой
// моделью не становится.
//
// Почему черновик всё-таки нужен. md4c — событийный разбор: он сообщает
// «начался блок», «текст», «кончился блок», и текст блока приходит кусками.
// Сложить блок целиком до того, как он кончился, нельзя, а положить в
// QTextDocument половину блока и дописать остаток — значит переразмечать его
// на каждом куске (замер: правка внутри блока кода на 31 480 знаков стоила
// 4257 мкс против 109 мкс в блоке на сотню). Поэтому блок собирается в стороне
// и уезжает в документ целиком.

#include "document_impl.h"

#include "document_pieces.h"

#include <cstdio>
#include "math_scan.h"
#include "note_header.h"

#include "md4c.h"

#include <QTextCursor>
#include <QTextDocument>

#include <algorithm>
#include <cassert>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace zametti {
namespace {

// Половина байтового диапазона черновика. Тот же приём, что был у арены, но
// местный: смещения живут ровно столько, сколько идёт разбор.
struct DraftRange {
    int32_t start = 0;
    int32_t end = 0;
    bool empty() const { return end <= start; }
    int32_t size() const { return end - start; }
    friend bool operator==(const DraftRange& a, const DraftRange& b) {
        return a.start == b.start && a.end == b.end;
    }
    friend bool operator!=(const DraftRange& a, const DraftRange& b) { return !(a == b); }
};

// Кусок строки в координатах черновика.
struct DraftRun {
    DraftRange text;
    DraftRange href;
    DraftRange title;
    uint8_t flags = 0;

    bool bold() const { return (flags & InlineBold) != 0; }
    bool italic() const { return (flags & InlineItalic) != 0; }
    bool strike() const { return (flags & InlineStrike) != 0; }
    bool code() const { return (flags & InlineCode) != 0; }
    bool image() const { return (flags & InlineImage) != 0; }
    bool comment() const { return (flags & InlineComment) != 0; }
    bool math() const { return (flags & InlineMath) != 0; }
    void set(uint8_t bit, bool on) { flags = uint8_t(on ? (flags | bit) : (flags & ~bit)); }
};

// Блок в координатах черновика.
struct DraftBlock {
    Kind kind = Kind::Paragraph;
    Marker marker = Marker::Bullet;
    bool checked = false;
    bool raw = false;
    int8_t headingLevel = 0;
    HtmlKind html = HtmlKind::Comment;
    int16_t level = -1;
    DraftRange text;
    DraftRange info;
    DraftRange inlines;   // ИНДЕКСЫ в Draft::runs, не байты
};

// Черновик разбора: байты и куски. Живёт один вызов.
struct Draft {
    std::string chars;
    std::vector<DraftRun> runs;

    DraftRange append(std::string_view bytes) {
        // Самоперекрытие законно: кусок черновика дописывается в его же конец.
        const int32_t start = int32_t(chars.size());
        chars.append(bytes);
        return DraftRange{start, int32_t(chars.size())};
    }
    std::string_view view(DraftRange r) const {
        if (r.empty()) return {};
        return std::string_view(chars).substr(size_t(r.start), size_t(r.size()));
    }
    std::string_view text(const DraftBlock& b) const { return view(b.text); }
    std::string_view info(const DraftBlock& b) const { return view(b.info); }
    std::string_view href(const DraftRun& r) const { return view(r.href); }
    std::string_view title(const DraftRun& r) const { return view(r.title); }

    // Дословный кусок. Дословное всегда кончается переводом строки: без него
    // последний блок файла без хвостового перевода не совпал бы сам с собой
    // после круга.
    DraftBlock newRaw(std::string_view bytes) {
        DraftBlock b;
        // Род дословного куска — всегда Paragraph по умолчанию: у дословного
        // рода нет вовсе, а wouldMerge смотрит на род, не спрашивая про raw.
        b.raw = true;
        b.text = append(bytes);
        if (b.text.empty() || chars[size_t(b.text.end) - 1] != '\n') {
            chars.push_back('\n');
            b.text.end = int32_t(chars.size());
        }
        return b;
    }

    // Дословный кусок — законченный HTML-комментарий: начинается с "<!--" и
    // кончается строкой с "-->" на конце. HTML-блок этого типа по CommonMark
    // кончается ровно на первой строке с "-->", поэтому такой кусок — один
    // целый комментарий, и внутри него не прячется ничего незакрытого. Куски с
    // "-->" в середине (обычные HTML-блоки) сюда не попадают — и не должны:
    // замерено, что "<div>" с "-->" внутри жадно съедает соседний код при
    // следующем чтении.
    bool isClosedHtmlComment(const DraftBlock& b) const {
        if (!b.raw) return false;
        std::string_view body = text(b);
        while (!body.empty() && (body.back() == '\n' || body.back() == '\r')) body.remove_suffix(1);
        return body.size() >= 7 && body.compare(0, 4, "<!--") == 0 &&
               body.compare(body.size() - 3, 3, "-->") == 0;
    }

    // Слипнутся ли блоки. Само правило общее и живёт в block_kind.h — здесь
    // только то, чего оно о черновике знать не может.
    bool wouldMerge(const DraftBlock& previous, const DraftBlock& next) const {
        return zametti::wouldMerge(previous.kind, previous.raw, isClosedHtmlComment(previous),
                                   next.kind, next.raw, next.level);
    }
};

constexpr size_t kNoOffset = static_cast<size_t>(-1);

struct Style {
    uint8_t flags = 0;   // InlineBold | InlineItalic | ... | InlineImage
    DraftRange href;
    DraftRange title;

    bool plain() const { return flags == 0 && href.empty(); }
};

// Диапазон исходника, занятый блоком. Хранится параллельно списку блоков, потому
// что самому IR эти сведения не нужны — они нужны только чтобы вырезать
// дословный кусок.
struct Extent {
    size_t minOff = kNoOffset;
    size_t maxOff = kNoOffset;   // смещение последнего байта, не следующего за ним
    bool   raw    = false;
    bool   fenced = false;       // блок кода огорожен — значит занимает ещё две строки
    size_t rawLines = 0;         // точная высота дословного куска, если она известна
    bool   wasHeading = false;   // кусок получился из заголовка — возможно, setext
};

struct Frame {
    MD_BLOCKTYPE type = MD_BLOCK_DOC;
    size_t docSizeAtEnter = 0;
    // Рубежи арены и спанов на входе в блок: по ним откатывается деградация.
    size_t charsAtEnter = 0;
    size_t spansAtEnter = 0;

    bool ordered = false;   // для UL/OL
    int  childIdx = 0;      // для LI: сколько блочных детей уже видели
    bool isTask = false;    // для LI
    char taskMark = ' ';    // для LI, осмысленно при isTask
};

struct Ctx {
    // buf — ИСХОДНИК, из него берутся все байты для вывода. md — копия, которую
    // видит md4c: в ней замаскированы строки внутри выключных формул (см.
    // maskDisplayMath). Длины равны, поэтому смещение в одной есть смещение и в
    // другой; вычисляются они по md, а читается всегда buf.
    const char* buf = nullptr;
    const char* md = nullptr;
    size_t len = 0;

    Draft draft;                 // черновик разбора: байты и куски
    std::vector<DraftBlock> doc;
    std::vector<Extent> ext;

    std::vector<Frame> stack;

    // Деградация в дословный кусок. Пока raw == true, содержимое не
    // разбирается, копятся только смещения.
    bool   raw = false;
    size_t rawMin = kNoOffset;
    size_t rawMax = kNoOffset;
    size_t rawLines = 0;
    bool   rawWasHeading = false;

    // Текущий листовой блок.
    bool   inLeaf = false;
    DraftBlock cur;
    std::string text;            // текст текущего блока, до переезда в арену
    size_t charsStart = 0;       // рубеж арены на начало текущего блока
    int32_t spanStart = 0;       // первый спан текущего блока в ir.spans
    bool   curFenced = false;
    bool   curRawAtEnd = false;  // блок разбирается, но уйдёт дословно
    size_t curMin = kNoOffset;
    size_t curMax = kNoOffset;

    // ФОРМУЛА СОБИРАЕТСЯ ПО СМЕЩЕНИЯМ, а не по тексту колбэка. md4c отдаёт
    // содержимое математики дословно (это и спасает `\,` и `\gamma`), но
    // перенос строки внутри формулы он превращает в пробел — а нам нужен
    // исходник байт в байт. Смещения дают его точно.
    bool   inMath = false;
    bool   mathDisplay = false;
    size_t mathMin = kNoOffset;
    size_t mathMax = kNoOffset;

    // Обратные кавычки встроенного кода не приходят ни одним колбэком, а
    // занимать могут отдельные строки ("``\nfoo\n``"). Ждём первого текста
    // внутри спана, чтобы отсчитать от него открывающий прогон.
    bool   codeSpanAwaitsText = false;

    std::vector<Style> styles;       // стек стилей; вершина — действующий
    std::vector<size_t> styleStart;  // где в text открылся каждый из них
    size_t runStart = 0;         // начало текущего прогона одного стиля в text

    // Литеральный маркер "[x] ", возвращаемый в текст для чекбокса внутри
    // нумерованного списка (правило 2).
    std::string pendingPrefix;
    size_t pendingPrefixOff = kNoOffset;
};

// Знак препинания ASCII — тот самый набор, перед которым в CommonMark косая
// является экранированием, а не буквой.
bool isAsciiPunct(char ch) {
    return std::strchr("!\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~", ch) != nullptr && ch != '\0';
}

void mergeOffset(size_t& lo, size_t& hi, size_t off, size_t size) {
    if (lo == kNoOffset || off < lo) lo = off;
    size_t last = off + (size > 0 ? size - 1 : 0);
    if (hi == kNoOffset || last > hi) hi = last;
}

void mergeRange(size_t& lo, size_t& hi, size_t olo, size_t ohi) {
    if (olo == kNoOffset) return;
    if (lo == kNoOffset || olo < lo) lo = olo;
    if (hi == kNoOffset || ohi > hi) hi = ohi;
}

int listDepthOf(const Ctx& c) {
    int d = 0;
    for (const Frame& f : c.stack)
        if (f.type == MD_BLOCK_UL || f.type == MD_BLOCK_OL) ++d;
    return d;
}

bool insideQuote(const Ctx& c) {
    for (const Frame& f : c.stack)
        if (f.type == MD_BLOCK_QUOTE) return true;
    return false;
}

const Frame* enclosingList(const Ctx& c) {
    for (size_t i = c.stack.size(); i-- > 0;)
        if (c.stack[i].type == MD_BLOCK_UL || c.stack[i].type == MD_BLOCK_OL) return &c.stack[i];
    return nullptr;
}

// Забыть всё, что набрано для текущего листа: текст, спаны и байты, ушедшие в
// арену от его имени (info-строка блока кода, адреса ссылок).
void dropLeaf(Ctx& c) {
    c.inLeaf = false;
    c.cur = DraftBlock{};
    c.text.clear();
    c.draft.runs.resize(size_t(c.spanStart));
    c.draft.chars.resize(c.charsStart);
    c.curMin = c.curMax = kNoOffset;
    c.curRawAtEnd = false;
    c.styles.clear();
    c.styleStart.clear();
}

// Перевести внешний открытый блок (ребёнка MD_BLOCK_DOC) в дословный кусок:
// выкинуть всё, что уже успели из него собрать, и дальше только копить смещения.
void demote(Ctx& c) {
    if (c.raw) return;
    if (c.stack.size() < 2) return;   // на уровне документа деградировать нечего

    c.raw = true;
    c.rawMin = kNoOffset;
    c.rawMax = kNoOffset;
    c.rawLines = 0;
    c.rawWasHeading = c.inLeaf && c.cur.kind == Kind::Heading;

    size_t keep = c.stack[1].docSizeAtEnter;
    for (size_t i = keep; i < c.doc.size(); ++i)
        mergeRange(c.rawMin, c.rawMax, c.ext[i].minOff, c.ext[i].maxOff);
    c.doc.resize(keep);
    c.ext.resize(keep);

    if (c.inLeaf) mergeRange(c.rawMin, c.rawMax, c.curMin, c.curMax);
    if (c.pendingPrefixOff != kNoOffset)
        mergeOffset(c.rawMin, c.rawMax, c.pendingPrefixOff, c.pendingPrefix.size());

    // Арена и спаны — назад к рубежу внешнего блока. Всё, что за ним, принадлежало
    // блокам, которые мы только что выбросили; у блоков, оставшихся в c.doc,
    // смещения лежат до рубежа.
    c.inLeaf = false;
    c.cur = DraftBlock{};
    c.text.clear();
    c.draft.runs.resize(c.stack[1].spansAtEnter);
    c.draft.chars.resize(c.stack[1].charsAtEnter);
    c.charsStart = c.stack[1].charsAtEnter;
    c.spanStart = int32_t(c.stack[1].spansAtEnter);
    c.curMin = c.curMax = kNoOffset;
    c.curRawAtEnd = false;
    c.styles.clear();
    c.styleStart.clear();
    c.pendingPrefix.clear();
    c.pendingPrefixOff = kNoOffset;
}

void flushRun(Ctx& c) {
    if (!c.inLeaf) return;
    const Style& st = c.styles.back();
    size_t end = c.text.size();
    if (end > c.runStart && !st.plain()) {
        DraftRun s;
        s.text = {int32_t(c.runStart), int32_t(end)};
        s.flags = st.flags;
        s.href = st.href;
        s.title = st.title;
        c.draft.runs.push_back(s);
    }
    c.runStart = end;
}

void startLeaf(Ctx& c, Kind kind, int headingLevel, int level) {
    c.inLeaf = true;
    c.cur = DraftBlock{};
    c.cur.kind = kind;
    c.cur.headingLevel = static_cast<int8_t>(headingLevel);
    c.cur.level = static_cast<int16_t>(level);
    c.text.clear();
    c.charsStart = c.draft.chars.size();
    c.spanStart = int32_t(c.draft.runs.size());
    c.curMin = c.curMax = kNoOffset;
    c.curFenced = false;
    c.curRawAtEnd = false;
    c.codeSpanAwaitsText = false;
    c.styles.assign(1, Style{});
    c.styleStart.assign(1, 0);
    c.runStart = 0;

    if (!c.pendingPrefix.empty()) {
        c.text = c.pendingPrefix;
        c.runStart = c.text.size();
        mergeOffset(c.curMin, c.curMax, c.pendingPrefixOff, c.pendingPrefix.size());
        c.pendingPrefix.clear();
        c.pendingPrefixOff = kNoOffset;
    }
}

// Стык двух спанов одного стиля смысла не несёт: разбор мог разбить прогон на
// части (вложенное выделение, экранированный символ), а модели важен только
// итоговый стиль. Без склейки IR перестал бы совпадать сам с собой после круга.
//
// Сравниваются ровно четыре признака и адрес — так было и до арены. Картинка и
// строчный комментарий в сравнение НЕ входят: у картинки адрес всегда свой, а
// два строчных комментария подряд склеиваются в один — это давнее поведение, и
// менять его здесь нельзя.
void mergeAdjacentSpans(Ctx& c) {
    constexpr uint8_t kStyleMask = InlineBold | InlineItalic | InlineStrike | InlineCode;
    std::vector<DraftRun>& all = c.draft.runs;
    const size_t from = size_t(c.spanStart);
    size_t write = from;
    for (size_t read = from; read < all.size(); ++read) {
        const DraftRun s = all[read];
        // Кусок нулевой длины ничего не помечает и потому не нужен — КРОМЕ
        // картинки: у неё содержимое не подпись, а сам снимок, и "![](фото)"
        // это законная запись. Пока и её выбрасывали здесь, такая строка
        // теряла картинку целиком.
        if (s.text.size() <= 0 && !s.image()) continue;
        if (write > from) {
            DraftRun& p = all[write - 1];
            // Картинку не сливаем ни с чем и ни с чем не сливаем: два снимка
            // подряд с одним адресом склеились бы в один, и второй пропал бы.
            if (!p.image() && !s.image() && p.text.end == s.text.start &&
                (p.flags & kStyleMask) == (s.flags & kStyleMask) &&
                c.draft.view(p.href) == c.draft.view(s.href)) {
                p.text.end = s.text.end;
                continue;
            }
        }
        all[write++] = s;
    }
    all.resize(write);
}

// Отмотать прогоны обратных кавычек назад от известного смещения. Нужно, когда
// содержимое встроенного кода md4c синтезировал целиком (перевод строки внутри
// кода приходит пробелом, без указателя в буфер) и якоря у спана нет вовсе.
// За пустую строку не заходим: там уже чужой блок.
size_t backOverTicks(const char* buf, size_t from) {
    size_t k = from;
    for (;;) {
        size_t j = k;
        while (j > 0 && (buf[j - 1] == ' ' || buf[j - 1] == '\t' || buf[j - 1] == '\r')) --j;
        if (j > 0 && buf[j - 1] == '\n') --j;
        while (j > 0 && (buf[j - 1] == ' ' || buf[j - 1] == '\t' || buf[j - 1] == '\r')) --j;
        size_t t = j;
        while (t > 0 && buf[t - 1] == '`') --t;
        if (t == j) break;
        k = t;
    }
    return k;
}

size_t forwardOverTicks(const char* buf, size_t len, size_t from) {
    size_t k = from;
    for (;;) {
        size_t j = k;
        while (j < len && (buf[j] == ' ' || buf[j] == '\t' || buf[j] == '\r')) ++j;
        if (j < len && buf[j] == '\n') ++j;
        while (j < len && (buf[j] == ' ' || buf[j] == '\t' || buf[j] == '\r')) ++j;
        size_t t = j;
        while (t < len && buf[t] == '`') ++t;
        if (t == j) break;
        k = t;
    }
    return k;
}

bool asciiSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

std::string_view trimAscii(std::string_view s) {
    size_t b = 0;
    while (b < s.size() && asciiSpace(s[b])) ++b;
    size_t e = s.size();
    while (e > b && asciiSpace(s[e - 1])) --e;
    return s.substr(b, e - b);
}

// Собранный HTML-блок — ровно один законченный комментарий? Тогда это
// Kind::Html: в тексте остаётся внутренность без скобок и крайних пробелов.
// Всё прочее (незакрытый комментарий, теги, два комментария в одном блоке —
// такого md4c не даёт, но проверка дешёвая) — дословно, как раньше.
bool adoptCommentLeaf(Ctx& c) {
    // Шапка метаданных "<!-- zametti" — не Kind::Html: её забирает liftMeta из
    // дословного блока, и её байты (включая неизвестные ключи) неприкосновенны.
    if (c.text.compare(0, 13, "<!-- zametti\n") == 0) return false;
    const std::string_view body = trimAscii(c.text);
    if (body.size() < 7) return false;
    if (body.compare(0, 4, "<!--") != 0) return false;
    if (body.compare(body.size() - 3, 3, "-->") != 0) return false;
    const std::string_view interior = trimAscii(body.substr(4, body.size() - 7));
    if (interior.find("-->") != std::string_view::npos) return false;

    // Внутренность вырезается на месте: она лежит внутри c.text, и присваивать
    // строке вид на саму себя нельзя.
    const size_t at = size_t(interior.data() - c.text.data());
    const size_t size = interior.size();
    c.text.erase(0, at);
    c.text.resize(size);
    c.cur.html = HtmlKind::Comment;
    return true;
}

// Текст листа уезжает в арену одним куском, спаны привязываются к блоку.
void commitLeaf(Ctx& c) {
    c.cur.text = c.draft.append(c.text);
    c.cur.inlines = {c.spanStart, int32_t(c.draft.runs.size())};
    c.doc.push_back(c.cur);
}

void endLeaf(Ctx& c) {
    if (!c.inLeaf) return;
    flushRun(c);
    mergeAdjacentSpans(c);

    const size_t spanCount = c.draft.runs.size() - size_t(c.spanStart);
    if (spanCount > 0 && c.curMin != kNoOffset) {
        const DraftRun& head = c.draft.runs[size_t(c.spanStart)];
        const DraftRun& tail = c.draft.runs.back();
        if (head.code() && head.text.start == 0)
            mergeOffset(c.curMin, c.curMax, backOverTicks(c.buf, c.curMin), 1);
        if (tail.code() && size_t(tail.text.end) == c.text.size()) {
            size_t e = forwardOverTicks(c.buf, c.len, c.curMax + 1);
            if (e > c.curMax + 1) mergeOffset(c.curMin, c.curMax, e - 1, 1);
        }
    }

    commitLeaf(c);
    c.ext.push_back(Extent{c.curMin, c.curMax, false, c.curFenced});
    c.cur = DraftBlock{};
    c.text.clear();
    c.inLeaf = false;
    c.charsStart = c.draft.chars.size();
    c.spanStart = int32_t(c.draft.runs.size());
    c.curMin = c.curMax = kNoOffset;
    c.styles.clear();
    c.styleStart.clear();
}

// Лист уходит дословным куском: содержимое выбрасывается, границы остаются —
// текст вырежет finishExtents прямо из исходника.
void endLeafAsRaw(Ctx& c, size_t rawLines) {
    const size_t minOff = c.curMin;
    const size_t maxOff = c.curMax;
    dropLeaf(c);
    c.doc.push_back(DraftBlock{});
    c.ext.push_back(Extent{minOff, maxOff, true, false, rawLines, false});
    c.charsStart = c.draft.chars.size();
    c.spanStart = int32_t(c.draft.runs.size());
}

// Атрибут md4c (href, info-строка и т.п.) — это исходный текст с разметкой на
// подстроки. Берём его дословно: так он и уйдёт обратно в вывод байт в байт.
// Нулевые символы представить нечем — сигналим о деградации, ничего не дописав.
bool attrToRange(Ctx& c, const MD_ATTRIBUTE& a, DraftRange& out) {
    out = DraftRange{};
    if (a.text == nullptr || a.size == 0) return true;
    if (a.substr_types != nullptr && a.substr_offsets != nullptr) {
        for (unsigned i = 0; a.substr_offsets[i] < a.size; ++i)
            if (a.substr_types[i] == MD_TEXT_NULLCHAR) return false;
    }
    out = c.draft.append(std::string_view(a.text, a.size));
    return true;
}

Marker markerForListItem(bool ordered, bool isTask) {
    if (ordered) return Marker::Ordered;
    return isTask ? Marker::Task : Marker::Bullet;
}

int enterBlock(MD_BLOCKTYPE type, void* detail, void* userdata) {
    Ctx& c = *static_cast<Ctx*>(userdata);

    Frame f;
    f.type = type;
    f.docSizeAtEnter = c.doc.size();
    f.charsAtEnter = c.draft.chars.size();
    f.spansAtEnter = c.draft.runs.size();

    if (type == MD_BLOCK_DOC) {
        c.stack.push_back(f);
        return 0;
    }

    if (c.raw) {
        c.stack.push_back(f);
        return 0;
    }

    switch (type) {
        case MD_BLOCK_QUOTE:
            // Модель плоская: у блока нет уровня цитирования, поэтому вложенная
            // цитата непредставима. Цитата внутри списка — тоже: элемент списка
            // держит ровно один абзац.
            if (insideQuote(c) || enclosingList(c) != nullptr) {
                c.stack.push_back(f);
                demote(c);
                return 0;
            }
            break;

        case MD_BLOCK_UL:
        case MD_BLOCK_OL:
            // Список внутри цитаты моделью не выражается: у Kind::Quote нет
            // содержимого сложнее абзаца.
            if (insideQuote(c)) {
                c.stack.push_back(f);
                demote(c);
                return 0;
            }
            f.ordered = (type == MD_BLOCK_OL);
            // Вложенный список — законный ребёнок элемента: так и выражается
            // вложенность. Считаем его, чтобы абзац после него деградировал.
            if (!c.stack.empty() && c.stack.back().type == MD_BLOCK_LI) {
                // Элемент-родитель закончился: выкладываем его до того, как
                // пойдут вложенные. Пустой текст — законное состояние ("- - a"):
                // без этого блока вложенный список всплыл бы на уровень выше.
                endLeaf(c);
                c.stack.back().childIdx++;
            }
            break;

        case MD_BLOCK_LI: {
            const auto* d = static_cast<const MD_BLOCK_LI_DETAIL*>(detail);
            const Frame* list = enclosingList(c);
            bool ordered = list != nullptr && list->ordered;

            // Правило 2: "[x]" после маркера нумерованного списка — литеральный
            // текст, а не чекбокс. Отключить tasklists нельзя (на них держатся
            // обычные "- [ ]"), поэтому маркер возвращаем в текст сами.
            // Берём срез исходника, а не синтезируем строку: иначе потеряются
            // верхний регистр X и лишние пробелы.
            if (d != nullptr && d->is_task && !ordered) {
                f.isTask = true;
                f.taskMark = d->task_mark;
            }

            if (d != nullptr && d->is_task && ordered) {
                size_t begin = static_cast<size_t>(d->task_mark_offset);
                if (begin == 0) return 0;   // невозможно, но лучше не падать
                begin -= 1;                 // сам task_mark_offset — символ между скобками
                size_t end = static_cast<size_t>(d->task_mark_offset) + 2;
                while (end < c.len && (c.buf[end] == ' ' || c.buf[end] == '\t')) ++end;
                if (end <= c.len && begin < end) {
                    c.pendingPrefix.assign(c.buf + begin, end - begin);
                    c.pendingPrefixOff = begin;
                }
            }

            // Плотные списки md4c в MD_BLOCK_P не заворачивает: текст приходит
            // прямо в элемент. Поэтому блок открываем здесь, а MD_BLOCK_P, если
            // он всё-таки придёт (разреженный список), просто продолжит его.
            startLeaf(c, Kind::ListItem, 0, listDepthOf(c) - 1);
            c.cur.marker = markerForListItem(ordered, f.isTask);
            c.cur.checked = f.isTask && (f.taskMark == 'x' || f.taskMark == 'X');
            break;
        }

        case MD_BLOCK_P: {
            Frame* li = (!c.stack.empty() && c.stack.back().type == MD_BLOCK_LI) ? &c.stack.back()
                                                                                 : nullptr;
            if (li != nullptr) {
                // Первый абзац пункта — это сам пункт: блок под него уже открыт
                // на входе в элемент (плотные списки md4c в MD_BLOCK_P не
                // заворачивает, и текст приходит прямо в элемент).
                //
                // Второй и следующие — обычные абзацы, стоящие внутри пункта.
                // Уровень говорит, внутри какого именно: сам пункт лежит на том
                // же уровне выше.
                if (li->childIdx != 0 || !c.inLeaf) {
                    endLeaf(c);
                    startLeaf(c, insideQuote(c) ? Kind::Quote : Kind::Paragraph, 0,
                              listDepthOf(c) - 1);
                    li->childIdx++;
                    break;
                }
                li->childIdx++;
            } else {
                startLeaf(c, insideQuote(c) ? Kind::Quote : Kind::Paragraph, 0, -1);
            }
            break;
        }

        case MD_BLOCK_H: {
            if (insideQuote(c)) {
                c.stack.push_back(f);
                demote(c);
                return 0;
            }
            if (!c.stack.empty() && c.stack.back().type == MD_BLOCK_LI) {
                c.stack.push_back(f);
                demote(c);
                return 0;
            }
            const auto* d = static_cast<const MD_BLOCK_H_DETAIL*>(detail);
            startLeaf(c, Kind::Heading, static_cast<int>(d->level), -1);
            break;
        }

        case MD_BLOCK_CODE: {
            const auto* d = static_cast<const MD_BLOCK_CODE_DETAIL*>(detail);
            bool inList = !c.stack.empty() && c.stack.back().type == MD_BLOCK_LI;
            // Внутри цитаты код по-прежнему дословен: цитата держит только
            // абзацы. А внутри пункта списка — обычный блок кода, у которого
            // есть уровень: он и говорит, внутри какого пункта тот стоит.
            //
            // Кроме одного случая: забор прямо на строке маркера ("- ```").
            // Тогда пункт и код делят одну строку, а границы блоков мы считаем
            // строками — двум блокам на одной строке взяться неоткуда. Такое
            // остаётся дословным, как было.
            Frame* codeLi = inList ? &c.stack.back() : nullptr;
            if (insideQuote(c) || (codeLi != nullptr && codeLi->childIdx == 0 && c.inLeaf)) {
                c.stack.push_back(f);
                demote(c);
                return 0;
            }
            // Пункт, внутри которого встал код, к этому времени ещё открыт:
            // закрываем его, иначе его текст пропал бы.
            if (codeLi != nullptr) {
                endLeaf(c);
                codeLi->childIdx++;
            }
            startLeaf(c, Kind::Code, 0, inList ? listDepthOf(c) - 1 : -1);
            c.curFenced = (d->fence_char != 0);
            // Нулевой символ в info-строке представить нечем — только тогда блок
            // уходит дословно. Разобрать его при этом всё равно надо: высота
            // забора считается по числу строк содержимого, а его на входе в
            // блок ещё нет.
            DraftRange info;
            c.curRawAtEnd = !attrToRange(c, d->info, info);
            c.cur.info = info;
            break;
        }

        case MD_BLOCK_HR:
            // Разделитель внутри цитаты или пункта плоской моделью не
            // выражается — дословно, как раньше. На верхнем уровне это лист без
            // единого текстового колбэка: finishExtents посадит его на первую
            // непустую строку после предыдущего блока.
            if (insideQuote(c) || enclosingList(c) != nullptr) {
                c.stack.push_back(f);
                demote(c);
                return 0;
            }
            startLeaf(c, Kind::Divider, 0, -1);
            break;

        case MD_BLOCK_TABLE: {
            // Единственная конструкция, у которой часть строк не даёт ни одного
            // текстового колбэка: строка-разделитель и ряды из пустых ячеек.
            // Восстанавливать её границы по смещениям нечем — зато высота в
            // строках известна точно: каждый ряд занимает ровно строку.
            const auto* d = static_cast<const MD_BLOCK_TABLE_DETAIL*>(detail);
            c.stack.push_back(f);
            demote(c);
            c.rawLines = d->head_row_count + 1 + d->body_row_count;
            return 0;
        }

        case MD_BLOCK_HTML: {
            // Кандидат в Kind::Html: текст соберётся колбэками MD_TEXT_HTML, а
            // на выходе из блока проверится, что это ровно один законченный
            // комментарий, — иначе дословно, как раньше. Внутри цитаты HTML
            // плоской моделью не выражается; внутри пункта — можно, по образцу
            // блока кода, кроме случая «блок на строке маркера».
            bool inList = !c.stack.empty() && c.stack.back().type == MD_BLOCK_LI;
            Frame* htmlLi = inList ? &c.stack.back() : nullptr;
            if (insideQuote(c) || (htmlLi != nullptr && htmlLi->childIdx == 0 && c.inLeaf)) {
                c.stack.push_back(f);
                demote(c);
                return 0;
            }
            if (htmlLi != nullptr) {
                endLeaf(c);
                htmlLi->childIdx++;
            }
            startLeaf(c, Kind::Html, 0, inList ? listDepthOf(c) - 1 : -1);
            break;
        }

        default:
            // HR, LATEXMATH и всё их содержимое — дословно.
            c.stack.push_back(f);
            demote(c);
            return 0;
    }

    c.stack.push_back(f);
    return 0;
}

int leaveBlock(MD_BLOCKTYPE type, void* detail, void* userdata) {
    (void)detail;
    Ctx& c = *static_cast<Ctx*>(userdata);
    if (c.stack.empty()) return 0;

    c.stack.pop_back();

    if (c.raw) {
        if (c.stack.size() == 1) {   // вышли из внешнего деградировавшего блока
            c.raw = false;
            c.doc.push_back(DraftBlock{});
            c.ext.push_back(Extent{c.rawMin, c.rawMax, true, false, c.rawLines, c.rawWasHeading});
            c.rawMin = c.rawMax = kNoOffset;
            c.rawLines = 0;
            c.rawWasHeading = false;
        }
        return 0;
    }

    switch (type) {
        case MD_BLOCK_CODE:
            if (c.inLeaf && c.curRawAtEnd) {
                size_t contentLines = 0;
                for (char ch : c.text)
                    if (ch == '\n') ++contentLines;
                endLeafAsRaw(c, contentLines + (c.curFenced ? 2 : 0));
                break;
            }
            endLeaf(c);
            break;
        case MD_BLOCK_H:
        case MD_BLOCK_HR:
            endLeaf(c);
            break;
        case MD_BLOCK_HTML:
            if (c.inLeaf && c.cur.kind == Kind::Html && adoptCommentLeaf(c)) {
                endLeaf(c);
                break;
            }
            // Не комментарий — дословно. Внутри пункта — деградация всего
            // пункта, как раньше (demote со стеком глубже документа). На
            // верхнем уровне стек уже схлопнут и demote бессилен — лист
            // превращается в дословный кусок руками, по образцу блока кода
            // с нечитаемой info-строкой.
            if (c.stack.size() >= 2) {
                demote(c);
                break;
            }
            if (c.inLeaf) endLeafAsRaw(c, 0);
            break;
        case MD_BLOCK_P:
            // Внутри элемента списка абзац лишь наполняет уже открытый блок:
            // закроет его выход из самого элемента. Внутри цитаты каждый абзац
            // сам по себе блок.
            if (c.stack.empty() || c.stack.back().type != MD_BLOCK_LI) endLeaf(c);
            break;
        case MD_BLOCK_LI:
            // Пустой элемент ("-" без текста) всё равно остаётся в документе:
            // иначе он молча исчезнет, а вложенный под ним список всплывёт
            // уровнем выше.
            endLeaf(c);
            c.pendingPrefix.clear();
            c.pendingPrefixOff = kNoOffset;
            break;
        default:
            break;
    }
    return 0;
}

int enterSpan(MD_SPANTYPE type, void* detail, void* userdata) {
    Ctx& c = *static_cast<Ctx*>(userdata);
    if (c.raw) return 0;
    if (!c.inLeaf) { demote(c); return 0; }

    flushRun(c);
    Style st = c.styles.back();
    if ((st.flags & InlineImage) != 0) {
        // Разметка внутри подписи картинки: плоскими спанами не выражается.
        demote(c);
        return 0;
    }

    switch (type) {
        case MD_SPAN_EM:
            st.flags |= InlineItalic;
            break;
        case MD_SPAN_STRONG:
            st.flags |= InlineBold;
            break;
        case MD_SPAN_DEL:
            st.flags |= InlineStrike;
            break;
        case MD_SPAN_CODE:
            st.flags |= InlineCode;
            c.codeSpanAwaitsText = true;
            break;
        case MD_SPAN_IMG: {
            // Картинка: подпись — текст спана, путь — href, заголовок — title.
            // Фрагмент в пути ("#w=300") — просто байты пути, их не трогаем.
            // Внутри подписи вложенной разметке взяться неоткуда: подпись у
            // картинки — обычный текст, а картинка внутри ссылки или разметки
            // плоским спаном не выражается — дословно.
            const auto* d = static_cast<const MD_SPAN_IMG_DETAIL*>(detail);
            DraftRange src;
            DraftRange title;
            if (!attrToRange(c, d->src, src) || !attrToRange(c, d->title, title) ||
                src.empty() || !st.plain() ||
                c.draft.view(title).find('"') != std::string_view::npos ||
                c.draft.view(title).find('\n') != std::string_view::npos) {
                demote(c);
                return 0;
            }
            st.flags |= InlineImage;
            st.href = src;
            st.title = title;
            break;
        }

        case MD_SPAN_A: {
            const auto* d = static_cast<const MD_SPAN_A_DETAIL*>(detail);
            DraftRange href;
            DraftRange title;
            if (!attrToRange(c, d->href, href) || !attrToRange(c, d->title, title) ||
                !title.empty() || href.empty()) {
                // Заголовок ссылки представить нечем; пустой href — тоже
                // (по нему ссылку не отличить от обычного текста).
                demote(c);
                return 0;
            }
            st.href = href;
            break;
        }
        case MD_SPAN_LATEXMATH:
        case MD_SPAN_LATEXMATH_DISPLAY:
            // Формула атомарна: ни разметки внутри, ни разметки вокруг. Внутри
            // жирного или ссылки плоским спаном её не выразить — дословно.
            if (!st.plain()) {
                demote(c);
                return 0;
            }
            c.inMath = true;
            c.mathDisplay = type == MD_SPAN_LATEXMATH_DISPLAY;
            c.mathMin = kNoOffset;
            c.mathMax = kNoOffset;
            // Стиль НЕ кладём на стек: спан формулы собирается целиком в
            // leaveSpan из исходника, и текста «внутри» у него нет.
            return 0;

        default:
            // WIKILINK, U — модели неизвестны.
            demote(c);
            return 0;
    }

    c.styles.push_back(st);
    c.styleStart.push_back(c.text.size());
    return 0;
}

int leaveSpan(MD_SPANTYPE type, void* detail, void* userdata) {
    (void)detail;
    Ctx& c = *static_cast<Ctx*>(userdata);
    if (c.raw) return 0;
    c.codeSpanAwaitsText = false;

    if (type == MD_SPAN_LATEXMATH || type == MD_SPAN_LATEXMATH_DISPLAY) {
        if (!c.inMath) return 0;
        c.inMath = false;
        if (!c.inLeaf || c.mathMin == kNoOffset) {
            demote(c);
            return 0;
        }
        // Границы формулы В ИСХОДНИКЕ: тело плюс доллары с обеих сторон.
        const size_t skip = c.mathDisplay ? 2 : 1;
        if (c.mathMin < skip || c.mathMax + 1 + skip > c.len) {
            demote(c);
            return 0;
        }
        const size_t open = c.mathMin - skip;
        // mathMax — ПОСЛЕДНИЙ байт тела (так считает mergeOffset), значит
        // закрывающий прогон начинается сразу за ним. Я на этом и попался:
        // принял mathMax за «за концом», брал не тот байт и деградировал в
        // дословный кусок весь абзац с формулой.
        const size_t close = c.mathMax + 1;   // первый доллар закрывающего прогона
        const std::string_view source(c.buf, c.len);
        if (source[open] != '$' || source[close] != '$') {
            demote(c);
            return 0;
        }
        const std::string_view literal = source.substr(open, close + skip - open);

        // КАНОН НАШ, А НЕ MD4C. У него границы считаются по флангам, как у
        // выделения, и «$ x + y$» он считает формулой, а pandoc (и GitHub) —
        // нет.
        flushRun(c);
        const size_t at = c.text.size();
        const bool ours = mathBordersOk(source, open, close, c.mathDisplay);
        if (ours) {
            // Формула — дословно: внутри математики markdown не действует.
            c.text.append(literal);
            DraftRun s;
            s.text = {int32_t(at), int32_t(c.text.size())};
            s.set(InlineMath, true);
            c.draft.runs.push_back(s);
        } else {
            // НЕ ФОРМУЛА — ЗНАЧИТ ОБЫЧНЫЙ ТЕКСТ, И ЭКРАНИРОВАНИЕ НАДО СНЯТЬ.
            //
            // Здесь я и посадил беду, которая испортила заметку владельца.
            // Сперва я клал в текст те же байты, что в файле: «файл от показа
            // не меняется». Но текст модели — это ТЕКСТ, а не markdown: при
            // записи `\` в нём экранируется заново. Одно открытие — и
            // `\gamma` становится `\\gamma`, следующее удваивает опять.
            // Заметка росла вдвое с каждым открытием, пока не раздулась
            // втрое от исходной.
            //
            // Снимаем экранирование ровно так, как это сделал бы md4c, если бы
            // не счёл кусок математикой: косая перед знаком препинания ASCII
            // исчезает, всё прочее буквально.
            for (size_t i = 0; i < literal.size(); ++i) {
                const char ch = literal[i];
                if (ch == '\\' && i + 1 < literal.size() && isAsciiPunct(literal[i + 1])) {
                    c.text.push_back(literal[i + 1]);
                    ++i;
                    continue;
                }
                c.text.push_back(ch);
            }
        }
        c.runStart = c.text.size();
        return 0;
    }

    if (!c.inLeaf || c.styles.size() < 2) return 0;

    // Закрывающий прогон кавычек: после содержимого, возможно через пробелы и
    // перевод строки.
    if (type == MD_SPAN_CODE && c.curMax != kNoOffset) {
        size_t k = c.curMax + 1;
        while (k < c.len && (c.buf[k] == ' ' || c.buf[k] == '\t' || c.buf[k] == '\r' ||
                             c.buf[k] == '\n'))
            ++k;
        while (k < c.len && c.buf[k] == '`') ++k;
        if (k > c.curMax + 1) mergeOffset(c.curMin, c.curMax, c.curMax, k - c.curMax);
    }

    // Пустой спан ("[](/url)") модель не выражает: спан нулевой длины ничего не
    // помечает и просто исчезнет вместе со ссылкой. Молча терять нельзя.
    //
    // КАРТИНКА — ИСКЛЮЧЕНИЕ, и это не поблажка, а разница по существу. У ссылки
    // содержимое спана и есть то, что видит человек: нет текста — нечего
    // показать. У картинки содержимое — сам СНИМОК, а подпись необязательна:
    // "![](фото.jxl)" это законная запись, и к иным снимкам подпись просто не
    // имеет смысла (правило владельца).
    //
    // Пока исключения не было, такая строка деградировала в дословный кусок:
    // в документ уезжали десять знаков «![](фото.jxl)» текстом, и фотография не
    // рисовалась вовсе.
    if (c.text.size() == c.styleStart.back()) {
        if ((c.styles.back().flags & InlineImage) == 0) {
            demote(c);
            return 0;
        }
        // Кусок нулевой длины: подписи нет, а адрес и заголовок есть — в них
        // вся картинка и заключена.
        DraftRun s;
        s.text = {int32_t(c.text.size()), int32_t(c.text.size())};
        s.flags = c.styles.back().flags;
        s.href = c.styles.back().href;
        s.title = c.styles.back().title;
        c.draft.runs.push_back(s);
        c.runStart = c.text.size();
        c.styles.pop_back();
        c.styleStart.pop_back();
        return 0;
    }

    flushRun(c);
    c.styles.pop_back();
    c.styleStart.pop_back();
    return 0;
}

int onText(MD_TEXTTYPE type, const MD_CHAR* text, MD_SIZE size, void* userdata) {
    Ctx& c = *static_cast<Ctx*>(userdata);

    // Часть колбэков приходит со статическими строками (" ", "\n", отступ кода),
    // указатель в буфер — только у настоящих кусков исходника.
    if (text >= c.md && text < c.md + c.len) {
        size_t off = static_cast<size_t>(text - c.md);
        if (c.raw)
            mergeOffset(c.rawMin, c.rawMax, off, size);
        else if (c.inLeaf)
            mergeOffset(c.curMin, c.curMax, off, size);
    }

    if (c.raw) return 0;
    if (!c.inLeaf) return 0;

    // Открывающий прогон кавычек стоит перед содержимым, возможно через пробелы
    // и перевод строки.
    if (c.codeSpanAwaitsText && text >= c.md && text < c.md + c.len) {
        c.codeSpanAwaitsText = false;
        size_t k = static_cast<size_t>(text - c.md);
        while (k > 0 && (c.buf[k - 1] == ' ' || c.buf[k - 1] == '\t' || c.buf[k - 1] == '\r' ||
                         c.buf[k - 1] == '\n'))
            --k;
        while (k > 0 && c.buf[k - 1] == '`') --k;
        mergeOffset(c.curMin, c.curMax, k, 1);
    }

    switch (type) {
        case MD_TEXT_NORMAL:
        case MD_TEXT_ENTITY:
        case MD_TEXT_CODE:
            // Из ИСХОДНИКА: указатель может смотреть в замаскированную копию, а
            // маска — не то, что владелец написал. Сущности (`&amp;`) приходят
            // отдельной строкой вне буфера, их берём как есть.
            if (text >= c.md && text < c.md + c.len)
                c.text.append(c.buf + (text - c.md), size);
            else
                c.text.append(text, size);
            break;
        case MD_TEXT_SOFTBR:
            // Содержимое setext-заголовка может занимать несколько строк, а ATX
            // — нет, и переносить его в вывод некуда. Такой заголовок остаётся
            // дословным.
            if (c.cur.kind == Kind::Heading) demote(c);
            else c.text.push_back('\n');
            break;
        case MD_TEXT_BR:
            // Жёсткий перенос ("  \n" или "\\\n") моделью не выражается.
            demote(c);
            break;
        case MD_TEXT_HTML:
            // Внутри HTML-блока текст просто копится: судьбу решит leaveBlock.
            if (c.inLeaf && c.cur.kind == Kind::Html) {
                c.text.append(text, size);
                break;
            }
            // Строчный комментарий в абзаце: приходит одним куском. Всё прочее
            // — дословно, как раньше.
            if (c.inLeaf && size >= 7 && std::strncmp(text, "<!--", 4) == 0 &&
                std::strncmp(text + size - 3, "-->", 3) == 0 && c.styles.back().plain() &&
                std::string_view(text + 4, size - 7).find("-->") == std::string_view::npos) {
                flushRun(c);
                const std::string_view interior = trimAscii(std::string_view(text + 4, size - 7));
                if (interior.empty()) { demote(c); break; }
                DraftRun s;
                s.text = {int32_t(c.text.size()), int32_t(c.text.size() + interior.size())};
                s.set(InlineComment, true);
                c.text.append(interior);
                c.draft.runs.push_back(s);
                c.runStart = c.text.size();
                break;
            }
            demote(c);
            break;
        case MD_TEXT_LATEXMATH:
            // Текст формулы не копим: он приедет из исходника по смещениям,
            // которые уже слиты выше. Колбэк md4c для многострочной формулы
            // отдаёт перенос строки пробелом, а нам нужен файл как есть.
            if (!c.inMath) demote(c);
            else if (text >= c.md && text < c.md + c.len)
                mergeOffset(c.mathMin, c.mathMax,
                            static_cast<size_t>(text - c.md), size);
            break;
        case MD_TEXT_NULLCHAR:
            demote(c);
            break;
    }
    return 0;
}

// --- восстановление границ блоков в исходнике ------------------------------

struct Lines {
    std::vector<size_t> start;
    size_t len = 0;

    size_t count() const { return start.size(); }
    size_t end(size_t i) const { return i + 1 < start.size() ? start[i + 1] : len; }
    size_t lineOf(size_t off) const {
        auto it = std::upper_bound(start.begin(), start.end(), off);
        return static_cast<size_t>(it - start.begin()) - 1;
    }
};

Lines buildLines(const char* buf, size_t len) {
    Lines l;
    l.len = len;
    l.start.push_back(0);
    for (size_t i = 0; i < len; ++i)
        if (buf[i] == '\n' && i + 1 < len) l.start.push_back(i + 1);
    return l;
}

size_t firstNonSpace(const char* buf, const Lines& l, size_t i, size_t& indent) {
    size_t p = l.start[i];
    size_t e = l.end(i);
    indent = 0;
    while (p < e && (buf[p] == ' ' || buf[p] == '\t')) { ++p; ++indent; }
    return p;
}

// Строка-забор: до трёх пробелов, потом три и больше '`' или '~'.
// maxIndent — сколько отступа забор может себе позволить. На верхнем уровне
// это 3 (глубже — уже отступный код), а внутри пункта списка забор стоит на
// колонке содержимого пункта плюс те же три: там предел не действует.
bool fenceLine(const char* buf, const Lines& l, size_t i, size_t maxIndent = 3) {
    size_t indent = 0;
    size_t p = firstNonSpace(buf, l, i, indent);
    size_t e = l.end(i);
    if (indent > maxIndent || p >= e) return false;
    char ch = buf[p];
    if (ch != '`' && ch != '~') return false;
    size_t j = p;
    while (j < e && buf[j] == ch) ++j;
    return j - p >= 3;
}

// Строка с маркером списка: "- ", "+ ", "* " или "12. ", "12) ".
bool listMarkerLine(const char* buf, const Lines& l, size_t i) {
    size_t indent = 0;
    size_t p = firstNonSpace(buf, l, i, indent);
    size_t e = l.end(i);
    if (p >= e) return false;
    char ch = buf[p];
    if (ch == '-' || ch == '+' || ch == '*')
        return p + 1 >= e || buf[p + 1] == ' ' || buf[p + 1] == '\t' || buf[p + 1] == '\n' ||
               buf[p + 1] == '\r';
    size_t j = p;
    while (j < e && buf[j] >= '0' && buf[j] <= '9') ++j;
    if (j == p || j >= e) return false;
    if (buf[j] != '.' && buf[j] != ')') return false;
    ++j;
    return j >= e || buf[j] == ' ' || buf[j] == '\t' || buf[j] == '\n' || buf[j] == '\r';
}

// Строка "> " без текста: внутри цитаты она разделяет абзацы и принадлежит ей,
// хотя текстовых колбэков по ней нет.
bool quoteOnlyLine(const char* buf, const Lines& l, size_t i) {
    size_t indent = 0;
    size_t p = firstNonSpace(buf, l, i, indent);
    size_t e = l.end(i);
    if (indent > 3 || p >= e || buf[p] != '>') return false;
    for (size_t k = p + 1; k < e; ++k)
        if (buf[k] != ' ' && buf[k] != '\t' && buf[k] != '\r' && buf[k] != '\n') return false;
    return true;
}

bool blankLine(const char* buf, const Lines& l, size_t i) {
    for (size_t p = l.start[i], e = l.end(i); p < e; ++p) {
        char ch = buf[p];
        if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n') return false;
    }
    return true;
}

// Смещения текстовых колбэков дают только «внутренность» блока: у таблицы это
// непустые ячейки, у HTML-блока — непустые строки. Расширяем до целых строк, а
// потом раздаём незанятые строки в промежутках между блоками: сначала вперёд от
// предыдущего блока, потом назад от следующего, останавливаясь на пустой
// строке. Так в дословный кусок попадают и строка-разделитель таблицы, и
// хвостовой ряд из пустых ячеек, и открывающая строка вроде "|   |   |".
void finishExtents(Ctx& c) {
    if (c.doc.empty()) return;

    Lines lines = buildLines(c.buf, c.len);
    size_t n = c.doc.size();

    std::vector<size_t> first(n), last(n);
    std::vector<bool> anchored(n, false);

    for (size_t i = 0; i < n; ++i) {
        if (c.ext[i].minOff != kNoOffset) {
            first[i] = lines.lineOf(c.ext[i].minOff);
            last[i] = lines.lineOf(c.ext[i].maxOff);
            anchored[i] = true;
        }
    }

    // Дальше — строго слева направо: каждая поправка опирается на уже уточнённую
    // границу предыдущего блока. Если этот порядок нарушить, блок без текстовых
    // колбэков сядет на строку, уже занятую соседом.
    for (size_t i = 0; i < n; ++i) {
        // Первая непустая строка после предыдущего блока — начало этого.
        size_t lo = (i == 0) ? 0 : last[i - 1] + 1;
        size_t hi = lines.count();
        for (size_t j = i + (anchored[i] ? 0 : 1); j < n; ++j)
            if (anchored[j]) { hi = first[j] + 1; break; }
        size_t at = lo;
        while (at < hi && at < lines.count() && blankLine(c.buf, lines, at)) ++at;
        if (at >= lines.count()) at = lines.count() - 1;

        // Блоки без единого текстового колбэка: тематический разделитель,
        // пустой заголовок, огороженный код без содержимого.
        if (!anchored[i]) {
            first[i] = last[i] = at;
            anchored[i] = true;
        }

        if (c.ext[i].raw) {
            // Многострочный setext-заголовок стал дословным, но подчёркивание
            // снизу — тоже его часть, а текстовых колбэков по нему нет.
            if (c.ext[i].wasHeading) {
                size_t p = lines.start[first[i]];
                size_t e = lines.end(first[i]);
                while (p < e && (c.buf[p] == ' ' || c.buf[p] == '\t')) ++p;
                bool atx = (p < e && c.buf[p] == '#');
                if (!atx && last[i] + 1 < lines.count()) last[i] += 1;
            }
            if (c.ext[i].rawLines > 0) {
                first[i] = at;
                last[i] = at + c.ext[i].rawLines - 1;
                if (last[i] >= lines.count()) last[i] = lines.count() - 1;
            }
            continue;
        }

        if (c.doc[i].kind == Kind::Code && c.ext[i].fenced) {
            // Забор занимает ещё по строке сверху и снизу. Отсчитывать верхнюю
            // как «строка перед первым текстом» нельзя: если содержимое
            // начинается с пустых строк, колбэков по ним не приходит и забор
            // уезжает вниз.
            size_t contentLines = 0;
            for (char ch : c.draft.text(c.doc[i]))
                if (ch == '\n') ++contentLines;
            // Забор ищется по виду строки, а не по «первой непустой после
            // предыдущего блока»: слева может стоять дословный кусок, чьи
            // границы ещё не уточнены, или определение ссылки, которого md4c
            // не показывает вовсе. Если содержимое есть — идём назад от него:
            // между ним и забором могут быть только пустые строки. Если нет —
            // только тогда ищем первый забор вперёд.
            // Забор кода внутри пункта отступает до колонки содержимого
            // пункта — предел «не глубже трёх» там не действует. Не узнав
            // собственный забор, блок присвоил бы себе чужой ниже по файлу, и
            // все границы поехали бы: ровно так лесенка из пунктов с кодом
            // задваивала содержимое (ficustut).
            const size_t fenceIndent =
                c.doc[i].level >= 0 ? ~size_t(0) : size_t(3);
            size_t fence = kNoOffset;
            if (anchored[i] && first[i] > at) {
                for (size_t k = first[i]; k-- > at;)
                    if (fenceLine(c.buf, lines, k, fenceIndent)) { fence = k; break; }
            }
            if (fence == kNoOffset) {
                for (size_t k = at; k < lines.count(); ++k)
                    if (fenceLine(c.buf, lines, k, fenceIndent)) { fence = k; break; }
            }
            first[i] = (fence != kNoOffset) ? fence : at;
            last[i] = first[i] + 1 + contentLines;
            if (last[i] >= lines.count()) last[i] = lines.count() - 1;
        } else if (c.doc[i].kind == Kind::Quote) {
            // Пустые строки цитаты ("> " без текста) — её часть, а не ничьи.
            while (last[i] + 1 < lines.count() && quoteOnlyLine(c.buf, lines, last[i] + 1))
                ++last[i];
            while (first[i] > 0 && first[i] - 1 > (i == 0 ? 0 : last[i - 1]) &&
                   quoteOnlyLine(c.buf, lines, first[i] - 1))
                --first[i];
        } else if (isList(c.doc[i].kind)) {
            // Маркер стоит на первой строке элемента, а текст может начаться
            // только со следующей ("-   \n  foo"). Без этой поправки строка с
            // маркером осталась бы ничьей и всплыла бы отдельным дословным
            // куском — то есть маркер задвоился бы.
            if (at < first[i] && listMarkerLine(c.buf, lines, at)) first[i] = at;
        } else if (c.doc[i].kind == Kind::Heading) {
            // Setext-заголовок подчёркнут отдельной строкой снизу. От ATX
            // отличается прямым взглядом на исходник: у ATX строка с '#'.
            size_t p = lines.start[first[i]];
            size_t e = lines.end(first[i]);
            while (p < e && (c.buf[p] == ' ' || c.buf[p] == '\t')) ++p;
            bool atx = (p < e && c.buf[p] == '#');
            if (!atx && last[i] + 1 < lines.count()) last[i] += 1;
        }
    }

    // Промежутки. Расширяем только дословные куски: у остальных вывод всё равно
    // порождается заново.
    for (size_t i = 0; i < n; ++i) {
        size_t gapLo = last[i] + 1;
        size_t gapHi = (i + 1 < n) ? first[i + 1] : lines.count();   // не включая
        if (c.ext[i].raw) {
            while (gapLo < gapHi && !blankLine(c.buf, lines, gapLo)) {
                last[i] = gapLo;
                ++gapLo;
            }
        }
        if (i + 1 < n && c.ext[i + 1].raw) {
            while (gapHi > gapLo && !blankLine(c.buf, lines, gapHi - 1)) {
                first[i + 1] = gapHi - 1;
                --gapHi;
            }
        }
    }
    if (c.ext[0].raw) {
        size_t at = first[0];
        while (at > 0 && !blankLine(c.buf, lines, at - 1)) {
            first[0] = at - 1;
            --at;
        }
    }

    // Дословный кусок, начинающийся с отступа, отдельным блоком не поставишь:
    // отступ привяжет его к соседу слева — например, сделает продолжением
    // последнего пункта списка, и после круга разбор поедет. Значит, кусок
    // должен забрать соседа себе, пока не начнётся с нулевой колонки.
    std::vector<bool> absorbed(n, false);
    for (size_t i = 0; i < n; ++i) {
        if (!c.ext[i].raw) continue;
        while (first[i] > 0) {
            char lead = c.buf[lines.start[first[i]]];
            if (lead != ' ' && lead != '\t') break;
            size_t j = i;
            bool found = false;
            while (j-- > 0) {
                if (!absorbed[j]) { found = true; break; }
            }
            if (!found) break;
            absorbed[j] = true;
            first[i] = first[j];
        }
    }

    // Зеркальная беда: если дословный кусок сам по себе список, то список,
    // идущий следом, при повторном разборе сольётся с ним в один — и группировка
    // блоков поедет. Такой кусок забирает соседей справа, пока они пункты.
    auto looksLikeList = [&](size_t a, size_t b) {
        for (size_t ln = a; ln <= b && ln < lines.count(); ++ln) {
            size_t p = lines.start[ln];
            size_t e = lines.end(ln);
            while (p < e && (c.buf[p] == ' ' || c.buf[p] == '\t')) ++p;
            if (p >= e) continue;
            char ch = c.buf[p];
            if ((ch == '-' || ch == '+' || ch == '*') && p + 1 < e &&
                (c.buf[p + 1] == ' ' || c.buf[p + 1] == '\t'))
                return true;
            size_t d = p;
            while (d < e && c.buf[d] >= '0' && c.buf[d] <= '9') ++d;
            if (d > p && d + 1 < e && (c.buf[d] == '.' || c.buf[d] == ')') &&
                (c.buf[d + 1] == ' ' || c.buf[d + 1] == '\t'))
                return true;
        }
        return false;
    };

    for (size_t i = 0; i < n; ++i) {
        if (!c.ext[i].raw || absorbed[i]) continue;
        if (!looksLikeList(first[i], last[i])) continue;
        for (size_t j = i + 1; j < n; ++j) {
            if (absorbed[j]) continue;
            if (c.ext[j].raw || !isList(c.doc[j].kind)) break;
            absorbed[j] = true;
            if (last[j] > last[i]) last[i] = last[j];
        }
    }

    for (size_t i = 0; i < n; ++i) {
        if (!c.ext[i].raw) continue;
        size_t b = lines.start[first[i]];
        size_t e = lines.end(last[i]);
        c.doc[i] = c.draft.newRaw(std::string_view(c.buf + b, e - b));
    }

    // Определения ссылок ("[1]: /a") md4c не отдаёт ни одним колбэком: он их
    // разрешает молча. Такие строки не покрыты ни одним блоком — и это
    // единственный оставшийся способ потерять байты, поэтому непокрытые
    // непустые строки становятся дословными кусками на своих местах.
    // Дословный кусок всегда заканчивается переводом строки: сериализатор его
    // всё равно допишет, и без этого IR последнего блока в файле без хвостового
    // перевода строки не совпал бы сам с собой после круга (это делает newRaw).
    auto rawFromLines = [&](size_t a, size_t b) {
        return c.draft.newRaw(
            std::string_view(c.buf + lines.start[a], lines.end(b) - lines.start[a]));
    };

    std::vector<DraftBlock> out;
    out.reserve(n + 2);
    size_t line = 0;

    // Плотный стык: между блоками не осталось ни одной пустой строки. Считаем
    // по номерам строк — они здесь уже уточнены до целых.
    size_t prevLast = 0;
    bool havePrev = false;
    auto push = [&](DraftBlock blk, size_t from, size_t to) {
        // Каждая пустая строка на стыке — свой блок. Столько же блоков, сколько
        // пустых строк в файле: пять подряд дадут пять блоков, и сериализатор
        // выведет их обратно один в один.
        if (havePrev && from > prevLast + 1) {
            for (size_t k = prevLast + 1; k < from; ++k) {
                DraftBlock gap;
                gap.kind = Kind::VSpace;
                out.push_back(gap);
            }
        }
        out.push_back(blk);
        prevLast = to;
        havePrev = true;
    };

    for (size_t i = 0; i < n; ++i) {
        if (absorbed[i]) continue;
        while (line < first[i]) {
            if (blankLine(c.buf, lines, line)) { ++line; continue; }
            size_t b = line;
            while (line < first[i] && !blankLine(c.buf, lines, line)) ++line;
            push(rawFromLines(b, line - 1), b, line - 1);
        }
        push(c.doc[i], first[i], last[i]);
        if (last[i] + 1 > line) line = last[i] + 1;
    }
    while (line < lines.count()) {
        if (blankLine(c.buf, lines, line)) { ++line; continue; }
        size_t b = line;
        while (line < lines.count() && !blankLine(c.buf, lines, line)) ++line;
        push(rawFromLines(b, line - 1), b, line - 1);
    }
    // Инвариант IR: между блоками, которые иначе слиплись бы, стоит VSpace.
    // Держим его здесь, а не при выводе: вставленная при выводе пустая строка
    // при следующем чтении стала бы блоком VSpace, которого в исходном IR не
    // было, и круг разошёлся бы.
    //
    // Такое случается на стыках, где наш канон длиннее исходника: незакрытый
    // забор дописывается закрывающим, и два блока кода, стоявшие вплотную,
    // разъезжаются.
    std::vector<DraftBlock> fixed;
    fixed.reserve(out.size() + 2);
    for (const DraftBlock& blk : out) {
        if (!fixed.empty() && c.draft.wouldMerge(fixed.back(), blk)) {
            DraftBlock gap;
            gap.kind = Kind::VSpace;
            fixed.push_back(gap);
        }
        fixed.push_back(blk);
    }
    c.doc = std::move(fixed);
}

// Первый блок с маркером zametti — метаданные заметки. Поднимаем их из
// блоков: редактор метаданные не видит вовсе, курсору встать некуда.
//
// Форма фиксированная: первая строка — ровно "<!-- zametti", закрывающая "-->"
// — своей строкой. Всё прочее — хвост после "-->", маркер в одну строку,
// комментарий не первым блоком — метаданными не является и остаётся дословным
// блоком (замерено пробником: md4c отдаёт такой комментарий одним блоком).
void liftMeta(Ctx& c, NoteHeader& header) {
    if (c.doc.empty()) return;
    if (!c.doc.front().raw) return;
    const std::string_view raw = c.draft.text(c.doc.front());
    constexpr std::string_view head = "<!-- zametti\n";
    constexpr std::string_view tail = "-->\n";
    if (raw.size() < head.size() + tail.size()) return;
    if (raw.compare(0, head.size(), head) != 0) return;
    if (raw.compare(raw.size() - tail.size(), tail.size(), tail) != 0) return;
    if (raw[raw.size() - tail.size() - 1] != '\n') return;

    std::vector<std::string> lines;
    size_t from = head.size();
    const size_t end = raw.size() - tail.size();
    while (from < end) {
        const size_t eol = raw.find('\n', from);
        lines.emplace_back(raw.substr(from, eol - from));
        from = eol + 1;
    }
    header.setPresent(true);
    header.setLines(std::move(lines));
    c.doc.erase(c.doc.begin());
    // Пустую строку после "-->" забираем с собой: в блоках ей стоять не за чем
    // — редактор показал бы пустую первую строку у каждой заметки.
    if (!c.doc.empty() && !c.doc.front().raw && c.doc.front().kind == Kind::VSpace) {
        header.setBlankAfter(true);
        c.doc.erase(c.doc.begin());
    }
}

// Выключная формула и markdown вокруг неё.
//
// Строки внутри `$$…$$` markdown разбирает как обычные строки документа — и они
// запросто оказываются разметкой. В корпусе разведки так и есть:
//
//     $$\left((x_1+1)(x_2-1)\right)
//     =
//     \bigl((x_1+1)(x_2-1)\bigl).$$
//
// Одинокое `=` — setext-подчёркивание, то есть заголовок, и решается это ДО
// всякой строчной разметки: формулы здесь не видит никто. Абзац уезжал в
// заголовок, и круг «разбор → запись» отдавал `# $$\left(...` — байты менялись
// молча. А внутри пункта списка вместе с ними уходил в дословный кусок весь
// список: девять формул корпуса.
//
// Вторая беда с той же стороны: `$$` на СВОЕЙ строке. У md4c границы формулы
// считаются по флангам, как у выделения, — за открывающими долларами не должно
// быть пробела. Поэтому
//
//     $$
//     \begin{aligned} … \end{aligned}
//     $$
//
// формулой он не считает вовсе, хотя pandoc и GitHub считают.
//
// Обе беды снимает одна маска: md4c показывается КОПИЯ исходника, в которой
// переносы строк ВНУТРИ выключной формулы заменены буквой. Формула становится
// для него однострочной: строк внутри нет — нет и разметки в них, а фланги
// сходятся. Длина не меняется ни на байт, значит смещения остаются смещениями в
// исходнике; байты для вывода берутся всегда из него (Ctx::buf), а не из копии
// (Ctx::md).
//
// Маскируются только выключные: у строчной, перенесённой через строку, перенос
// — часть её вида, и трогать его незачем.
//
// Замаскировать формулу ВНУТРИ блока кода значило бы разрушить сам блок, а его
// содержимое буквально. Поэтому огороженные блоки (``` и ~~~) пропускаются:
// сканер канона про них не знает, а показывать формулу в примере кода никто и
// не просит. Отступный код (четыре пробела) так не отличить от пункта списка —
// это остаётся известным краем.
std::vector<std::pair<size_t, size_t>> fencedRegions(std::string_view text) {
    std::vector<std::pair<size_t, size_t>> found;
    size_t line = 0;
    size_t openAt = std::string_view::npos;
    char fence = 0;
    size_t fenceLen = 0;
    while (line < text.size()) {
        size_t eol = text.find('\n', line);
        if (eol == std::string_view::npos) eol = text.size();
        size_t p = line;
        size_t indent = 0;
        while (p < eol && (text[p] == ' ' || text[p] == '\t')) { ++p; ++indent; }
        if (indent <= 3 && p < eol && (text[p] == '`' || text[p] == '~')) {
            const char ch = text[p];
            size_t run = 0;
            while (p + run < eol && text[p + run] == ch) ++run;
            if (run >= 3) {
                if (openAt == std::string_view::npos) {
                    openAt = line;
                    fence = ch;
                    fenceLen = run;
                } else if (ch == fence && run >= fenceLen) {
                    found.emplace_back(openAt, eol);
                    openAt = std::string_view::npos;
                }
            }
        }
        line = eol + 1;
    }
    if (openAt != std::string_view::npos) found.emplace_back(openAt, text.size());
    return found;
}

std::string maskDisplayMath(std::string_view text) {
    std::string masked;
    std::vector<std::pair<size_t, size_t>> fenced;
    bool fencedKnown = false;
    for (const MathSpan& span : scanMath(text)) {
        if (!span.display) continue;
        const size_t from = size_t(span.start);
        const size_t to = size_t(span.end);
        const std::string_view body = text.substr(from, to - from);
        if (body.find('\n') == std::string_view::npos) continue;
        // ПУСТАЯ СТРОКА ВНУТРИ — не маскируем. Канон ищет закрывающие `$$` хоть
        // через сорок строк, и одинокая пара долларов в разных абзацах даёт
        // «формулу» в полдокумента. Замаскировать её переносы значило бы слепить
        // эти абзацы в один и разрушить разметку между ними; формула из двух
        // абзацев не бывает, а вот случайный доллар — бывает.
        bool hasBlank = false;
        for (size_t i = 0; i + 1 < body.size() && !hasBlank; ++i) {
            if (body[i] != '\n') continue;
            size_t k = i + 1;
            while (k < body.size() && (body[k] == ' ' || body[k] == '\t' || body[k] == '\r')) ++k;
            hasBlank = k < body.size() && body[k] == '\n';
        }
        if (hasBlank) continue;

        if (!fencedKnown) {
            fenced = fencedRegions(text);
            fencedKnown = true;
        }
        bool inCode = false;
        for (const auto& region : fenced)
            if (from >= region.first && from < region.second) { inCode = true; break; }
        if (inCode) continue;

        if (masked.empty()) masked.assign(text);
        for (size_t i = from; i < to; ++i)
            if (masked[i] == '\n' || masked[i] == '\r') masked[i] = 'x';
    }
    return masked;
}


// АБЗАЦ, КОТОРЫЙ ЦЕЛИКОМ ЯВЛЯЕТСЯ ФОРМУЛОЙ, — не абзац, а блок-формула.
//
// Решение владельца: строчная формула остаётся спаном, выключная становится
// объектом. У блока два следствия, и оба нужны:
//
//   * литеральное литерально. Спан внутри абзаца попадал под правила
//     нормализации абзацев, и одно из них (ведущие пробелы → неразрывные)
//     молча испортило владельцу заметку — матрица уехала в файл с U+00A0;
//   * объектом на экране становится ровно то, что и должно: формула целиком, а
//     не строка текста, в которой она случайно оказалась одна.
//
// Одиночные доллары сюда тоже попадают: `$f(x) = …$` отдельной строкой — это
// выключная формула по замыслу автора, и всеми читалками она показывается
// именно так. БАЙТЫ ПРИ ЭТОМ НЕ МЕНЯЮТСЯ: текст блока — тот же исходник с теми
// же долларами, и запись отдаёт его дословно.
void liftMath(Ctx& c) {
    for (DraftBlock& b : c.doc) {
        if (b.raw || b.kind != Kind::Paragraph) continue;
        if (b.inlines.size() != 1) continue;
        const DraftRun& run = c.draft.runs[size_t(b.inlines.start)];
        if (!run.math()) continue;
        // Формула — весь текст блока, без хвостов по краям.
        if (run.text.start != 0 || run.text.end != b.text.size()) continue;
        b.kind = Kind::Math;
        b.inlines = DraftRange{};
    }
}

}  // namespace

// Разбор в логические блоки. Ступень внутренняя: наружу из ядра не выходит, а
// внутри его зовут двое — ZDocument::loadMarkdown (ниже) и умирающий мостик к
// представлению.
void parsePieces(std::string_view markdown, std::vector<Piece>& blocks, NoteHeader& header) {
    blocks.clear();
    header = NoteHeader{};
    if (markdown.empty()) return;

    Ctx c;
    const std::string masked = maskDisplayMath(markdown);
    c.buf = markdown.data();
    c.md = masked.empty() ? markdown.data() : masked.data();
    c.len = markdown.size();

    MD_PARSER parser{};
    parser.abi_version = 0;
    // Голые ссылки (https://…, www.…, почта) — часть диалекта GitHub, и в
    // заметках они встречаются куда чаще, чем размеченные вручную.
    // LATEXMATHSPANS — ради формул этапа 16. Он даёт содержимое математики
    // ДОСЛОВНО, и это главное: без него md4c разрешает экранирование внутри
    // формулы, и `$\int_0^1 x^2 \, dx$` возвращается из круга как
    // `$\int_0^1 x^2 , dx$` — тонкий пробел исчезает молча. Границы у md4c
    // при этом свои, и наш канон проверяется поверх (leaveSpan).
    parser.flags = MD_FLAG_TABLES | MD_FLAG_STRIKETHROUGH | MD_FLAG_TASKLISTS |
                   MD_FLAG_PERMISSIVEAUTOLINKS | MD_FLAG_LATEXMATHSPANS;
    parser.enter_block = enterBlock;
    parser.leave_block = leaveBlock;
    parser.enter_span = enterSpan;
    parser.leave_span = leaveSpan;
    parser.text = onText;
    parser.debug_log = nullptr;
    parser.syntax = nullptr;

    // Черновик вмещает текст блоков, дословные куски, info-строки и адреса. Всё
    // это — куски исходника, и суммарно больше него не выходит (замер на
    // корпусе: коэффициент см. docs/zametti-m5-report.md). Резерв с запасом
    // избавляет разбор от перекладываний черновика целиком.
    c.draft.chars.reserve(draftReserveFor(c.len));

    md_parse(c.md, static_cast<MD_SIZE>(c.len), &parser, &c);

    endLeaf(c);
    finishExtents(c);
    liftMath(c);
    liftMeta(c, header);

    // Черновик кончился. Дальше живут только логические блоки: смещения
    // превращаются в собственные байты блока, и на этом разбор о черновике
    // забывает.
    blocks.reserve(c.doc.size());
    for (const DraftBlock& b : c.doc) {
        Piece piece;
        piece.kind = b.kind;
        piece.marker = b.marker;
        piece.html = b.html;
        piece.level = b.level;
        piece.headingLevel = b.headingLevel;
        piece.checked = b.checked;
        piece.raw = b.raw;
        piece.info = std::string(c.draft.info(b));
        piece.text = std::string(c.draft.text(b));
        piece.trailingNewline = !piece.text.empty() && piece.text.back() == '\n';
        piece.runs.reserve(size_t(b.inlines.size()));
        for (int32_t i = b.inlines.start; i < b.inlines.end; ++i) {
            const DraftRun& src = c.draft.runs[size_t(i)];
            Run run;
            run.start = src.text.start;
            run.end = src.text.end;
            run.flags = src.flags;
            run.href = std::string(c.draft.href(src));
            run.title = std::string(c.draft.title(src));
            piece.runs.push_back(std::move(run));
        }
        blocks.push_back(std::move(piece));
    }
}

}  // namespace zametti
