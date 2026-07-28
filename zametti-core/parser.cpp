// md4c → IR.
//
// md4c — SAX-парсер и границ блоков в исходнике напрямую не отдаёт. Поэтому
// смещения считаются арифметикой указателей в текстовых колбэках (только для
// указателей, реально лежащих внутри входного буфера — часть колбэков приходит
// со статическими строками вроде " " и "\n"), накапливаются в min/max на блок,
// а потом расширяются до границ строк и до соседей. Подробности — в
// finishExtents() ниже.
//
// Всё, что не ложится в IR, деградирует в rawSource целиком по внешнему блоку:
// картинка внутри абзаца делает rawSource из абзаца, а таблица или цитата — из
// себя целиком. Это грубо, зато байты не теряются никогда.

#include "parser.h"

#include "md4c.h"

#include <algorithm>
#include <cassert>
#include <cstring>

namespace zametti {
namespace {

constexpr size_t kNoOffset = static_cast<size_t>(-1);

struct Style {
    bool bold = false;
    bool italic = false;
    bool strike = false;
    bool code = false;
    std::string href;

    bool plain() const { return !bold && !italic && !strike && !code && href.empty(); }
};

// Диапазон исходника, занятый блоком. Хранится параллельно Document, потому что
// самому IR эти сведения не нужны — они нужны только чтобы вырезать rawSource.
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

    bool ordered = false;   // для UL/OL
    int  childIdx = 0;      // для LI: сколько блочных детей уже видели
    bool isTask = false;    // для LI
    char taskMark = ' ';    // для LI, осмысленно при isTask
};

struct Ctx {
    const char* buf = nullptr;
    size_t len = 0;

    Document doc;
    std::vector<Extent> ext;

    std::vector<Frame> stack;

    // Деградация в rawSource. Пока raw == true, содержимое не разбирается,
    // копятся только смещения.
    bool   raw = false;
    size_t rawMin = kNoOffset;
    size_t rawMax = kNoOffset;
    size_t rawLines = 0;
    bool   rawWasHeading = false;

    // Текущий листовой блок.
    bool   inLeaf = false;
    Block  cur;
    bool   curFenced = false;
    bool   curRawAtEnd = false;  // блок разбирается, но уйдёт дословно
    size_t curMin = kNoOffset;
    size_t curMax = kNoOffset;

    // Обратные кавычки встроенного кода не приходят ни одним колбэком, а
    // занимать могут отдельные строки ("``\nfoo\n``"). Ждём первого текста
    // внутри спана, чтобы отсчитать от него открывающий прогон.
    bool   codeSpanAwaitsText = false;

    std::vector<Style> styles;       // стек стилей; вершина — действующий
    std::vector<size_t> styleStart;  // где в cur.text открылся каждый из них
    size_t runStart = 0;         // начало текущего прогона одного стиля в cur.text

    // Литеральный маркер "[x] ", возвращаемый в текст для чекбокса внутри
    // нумерованного списка (правило 2).
    std::string pendingPrefix;
    size_t pendingPrefixOff = kNoOffset;
};

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

// Перевести внешний открытый блок (ребёнка MD_BLOCK_DOC) в rawSource: выкинуть
// всё, что уже успели из него собрать, и дальше только копить смещения.
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

    c.inLeaf = false;
    c.cur = Block{};
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
    size_t end = c.cur.text.size();
    if (end > c.runStart && !st.plain()) {
        Span s;
        s.offset = static_cast<int>(c.runStart);
        s.length = static_cast<int>(end - c.runStart);
        s.bold = st.bold;
        s.italic = st.italic;
        s.strike = st.strike;
        s.code = st.code;
        s.href = st.href;
        c.cur.inlines.push_back(std::move(s));
    }
    c.runStart = end;
}

void startLeaf(Ctx& c, Kind kind, int headingLevel, int level) {
    c.inLeaf = true;
    c.cur = Block{};
    c.cur.kind = kind;
    c.cur.headingLevel = headingLevel;
    c.cur.level = level;
    c.curMin = c.curMax = kNoOffset;
    c.curFenced = false;
    c.curRawAtEnd = false;
    c.codeSpanAwaitsText = false;
    c.styles.assign(1, Style{});
    c.styleStart.assign(1, 0);
    c.runStart = 0;

    if (!c.pendingPrefix.empty()) {
        c.cur.text = c.pendingPrefix;
        c.runStart = c.cur.text.size();
        mergeOffset(c.curMin, c.curMax, c.pendingPrefixOff, c.pendingPrefix.size());
        c.pendingPrefix.clear();
        c.pendingPrefixOff = kNoOffset;
    }
}

// Стык двух спанов одного стиля смысла не несёт: разбор мог разбить прогон на
// части (вложенное выделение, экранированный символ), а модели важен только
// итоговый стиль. Без склейки IR перестал бы совпадать сам с собой после круга.
void mergeAdjacentSpans(std::vector<Span>& spans) {
    std::vector<Span> merged;
    merged.reserve(spans.size());
    for (Span& s : spans) {
        if (s.length <= 0) continue;
        if (!merged.empty()) {
            Span& p = merged.back();
            if (p.offset + p.length == s.offset && p.bold == s.bold && p.italic == s.italic &&
                p.strike == s.strike && p.code == s.code && p.href == s.href) {
                p.length += s.length;
                continue;
            }
        }
        merged.push_back(std::move(s));
    }
    spans.swap(merged);
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

void endLeaf(Ctx& c) {
    if (!c.inLeaf) return;
    flushRun(c);
    mergeAdjacentSpans(c.cur.inlines);

    if (!c.cur.inlines.empty() && c.curMin != kNoOffset) {
        const Span& head = c.cur.inlines.front();
        const Span& tail = c.cur.inlines.back();
        if (head.code && head.offset == 0)
            mergeOffset(c.curMin, c.curMax, backOverTicks(c.buf, c.curMin), 1);
        if (tail.code && static_cast<size_t>(tail.offset + tail.length) == c.cur.text.size()) {
            size_t e = forwardOverTicks(c.buf, c.len, c.curMax + 1);
            if (e > c.curMax + 1) mergeOffset(c.curMin, c.curMax, e - 1, 1);
        }
    }

    c.doc.push_back(std::move(c.cur));
    c.ext.push_back(Extent{c.curMin, c.curMax, false, c.curFenced});
    c.cur = Block{};
    c.inLeaf = false;
    c.curMin = c.curMax = kNoOffset;
    c.styles.clear();
    c.styleStart.clear();
}

// Атрибут md4c (href, info-строка и т.п.) — это исходный текст с разметкой на
// подстроки. Берём его дословно: так он и уйдёт обратно в вывод байт в байт.
// Нулевые символы представить нечем — сигналим о деградации.
bool attrToString(const MD_ATTRIBUTE& a, std::string& out) {
    out.clear();
    if (a.text == nullptr || a.size == 0) return true;
    if (a.substr_types != nullptr && a.substr_offsets != nullptr) {
        for (unsigned i = 0; a.substr_offsets[i] < a.size; ++i)
            if (a.substr_types[i] == MD_TEXT_NULLCHAR) return false;
    }
    out.assign(a.text, a.size);
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
            std::string info;
            bool inList = !c.stack.empty() && c.stack.back().type == MD_BLOCK_LI;
            if (inList || insideQuote(c)) {
                c.stack.push_back(f);
                demote(c);
                return 0;
            }
            startLeaf(c, Kind::Code, 0, -1);
            c.curFenced = (d->fence_char != 0);
            // Нулевой символ в info-строке представить нечем — только тогда блок
            // уходит дословно. Разобрать его при этом всё равно надо: высота
            // забора считается по числу строк содержимого, а его на входе в
            // блок ещё нет.
            c.curRawAtEnd = !attrToString(d->info, info);
            c.cur.info = std::move(info);
            break;
        }

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

        default:
            // HR, HTML и всё их содержимое — дословно.
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
            Block b;
            c.doc.push_back(std::move(b));
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
                for (char ch : c.cur.text)
                    if (ch == '\n') ++contentLines;
                c.doc.push_back(Block{});
                c.ext.push_back(Extent{c.curMin, c.curMax, true, false,
                                       contentLines + (c.curFenced ? 2 : 0), false});
                c.cur = Block{};
                c.inLeaf = false;
                c.curMin = c.curMax = kNoOffset;
                c.curRawAtEnd = false;
                c.styles.clear();
                c.styleStart.clear();
                break;
            }
            endLeaf(c);
            break;
        case MD_BLOCK_H:
            endLeaf(c);
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

    switch (type) {
        case MD_SPAN_EM:
            st.italic = true;
            break;
        case MD_SPAN_STRONG:
            st.bold = true;
            break;
        case MD_SPAN_DEL:
            st.strike = true;
            break;
        case MD_SPAN_CODE:
            st.code = true;
            c.codeSpanAwaitsText = true;
            break;
        case MD_SPAN_A: {
            const auto* d = static_cast<const MD_SPAN_A_DETAIL*>(detail);
            std::string href;
            std::string title;
            if (!attrToString(d->href, href) || !attrToString(d->title, title) ||
                !title.empty() || href.empty()) {
                // Заголовок ссылки представить нечем; пустой href — тоже
                // (по нему ссылку не отличить от обычного текста).
                demote(c);
                return 0;
            }
            st.href = std::move(href);
            break;
        }
        default:
            // IMG, LATEXMATH, WIKILINK, U — модели неизвестны.
            demote(c);
            return 0;
    }

    c.styles.push_back(std::move(st));
    c.styleStart.push_back(c.cur.text.size());
    return 0;
}

int leaveSpan(MD_SPANTYPE type, void* detail, void* userdata) {
    (void)detail;
    Ctx& c = *static_cast<Ctx*>(userdata);
    if (c.raw) return 0;
    c.codeSpanAwaitsText = false;
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

    // Пустой спан ("[](/url)") модель не выражает: Span нулевой длины ничего не
    // помечает и просто исчезнет вместе со ссылкой. Молча терять нельзя.
    if (c.cur.text.size() == c.styleStart.back()) {
        demote(c);
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
    if (text >= c.buf && text < c.buf + c.len) {
        size_t off = static_cast<size_t>(text - c.buf);
        if (c.raw)
            mergeOffset(c.rawMin, c.rawMax, off, size);
        else if (c.inLeaf)
            mergeOffset(c.curMin, c.curMax, off, size);
    }

    if (c.raw) return 0;
    if (!c.inLeaf) return 0;

    // Открывающий прогон кавычек стоит перед содержимым, возможно через пробелы
    // и перевод строки.
    if (c.codeSpanAwaitsText && text >= c.buf && text < c.buf + c.len) {
        c.codeSpanAwaitsText = false;
        size_t k = static_cast<size_t>(text - c.buf);
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
            c.cur.text.append(text, size);
            break;
        case MD_TEXT_SOFTBR:
            // Содержимое setext-заголовка может занимать несколько строк, а ATX
            // — нет, и переносить его в вывод некуда. Такой заголовок остаётся
            // дословным.
            if (c.cur.kind == Kind::Heading) demote(c);
            else c.cur.text.push_back('\n');
            break;
        case MD_TEXT_BR:
            // Жёсткий перенос ("  \n" или "\\\n") моделью не выражается.
            demote(c);
            break;
        case MD_TEXT_NULLCHAR:
        case MD_TEXT_HTML:
        case MD_TEXT_LATEXMATH:
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
bool fenceLine(const char* buf, const Lines& l, size_t i) {
    size_t indent = 0;
    size_t p = firstNonSpace(buf, l, i, indent);
    size_t e = l.end(i);
    if (indent > 3 || p >= e) return false;
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
// строке. Так в rawSource попадают и строка-разделитель таблицы, и хвостовой
// ряд из пустых ячеек, и открывающая строка вроде "|   |   |".
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
            for (char ch : c.doc[i].text)
                if (ch == '\n') ++contentLines;
            // Забор ищется по виду строки, а не по «первой непустой после
            // предыдущего блока»: слева может стоять дословный кусок, чьи
            // границы ещё не уточнены, или определение ссылки, которого md4c
            // не показывает вовсе. Если содержимое есть — идём назад от него:
            // между ним и забором могут быть только пустые строки. Если нет —
            // только тогда ищем первый забор вперёд.
            size_t fence = kNoOffset;
            if (anchored[i] && first[i] > at) {
                for (size_t k = first[i]; k-- > at;)
                    if (fenceLine(c.buf, lines, k)) { fence = k; break; }
            }
            if (fence == kNoOffset) {
                for (size_t k = at; k < lines.count(); ++k)
                    if (fenceLine(c.buf, lines, k)) { fence = k; break; }
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

    // Промежутки. Расширяем только rawSource-блоки: у остальных вывод всё равно
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
        c.doc[i].rawSource.assign(c.buf + b, e - b);
        if (c.doc[i].rawSource.empty() || c.doc[i].rawSource.back() != '\n')
            c.doc[i].rawSource.push_back('\n');
    }

    // Определения ссылок ("[1]: /a") md4c не отдаёт ни одним колбэком: он их
    // разрешает молча. Такие строки не покрыты ни одним блоком — и это
    // единственный оставшийся способ потерять байты, поэтому непокрытые
    // непустые строки становятся rawSource-блоками на своих местах.
    // Дословный кусок всегда заканчивается переводом строки: сериализатор его
    // всё равно допишет, и без этого IR последнего блока в файле без хвостового
    // перевода строки не совпал бы сам с собой после круга.
    auto rawFromLines = [&](size_t a, size_t b) {
        Block blk;
        blk.rawSource.assign(c.buf + lines.start[a], lines.end(b) - lines.start[a]);
        if (blk.rawSource.empty() || blk.rawSource.back() != '\n') blk.rawSource.push_back('\n');
        return blk;
    };

    Document out;
    out.reserve(n + 2);
    size_t line = 0;

    // Плотный стык: между блоками не осталось ни одной пустой строки. Считаем
    // по номерам строк — они здесь уже уточнены до целых.
    size_t prevLast = 0;
    bool havePrev = false;
    auto emit = [&](Block blk, size_t from, size_t to) {
        // Каждая пустая строка на стыке — свой блок. Столько же блоков, сколько
        // пустых строк в файле: пять подряд дадут пять блоков, и сериализатор
        // выведет их обратно один в один.
        if (havePrev && from > prevLast + 1) {
            for (size_t k = prevLast + 1; k < from; ++k) {
                Block gap;
                gap.kind = Kind::VSpace;
                out.push_back(std::move(gap));
            }
        }
        out.push_back(std::move(blk));
        prevLast = to;
        havePrev = true;
    };

    for (size_t i = 0; i < n; ++i) {
        if (absorbed[i]) continue;
        while (line < first[i]) {
            if (blankLine(c.buf, lines, line)) { ++line; continue; }
            size_t b = line;
            while (line < first[i] && !blankLine(c.buf, lines, line)) ++line;
            emit(rawFromLines(b, line - 1), b, line - 1);
        }
        emit(std::move(c.doc[i]), first[i], last[i]);
        if (last[i] + 1 > line) line = last[i] + 1;
    }
    while (line < lines.count()) {
        if (blankLine(c.buf, lines, line)) { ++line; continue; }
        size_t b = line;
        while (line < lines.count() && !blankLine(c.buf, lines, line)) ++line;
        emit(rawFromLines(b, line - 1), b, line - 1);
    }
    // Инвариант IR: между блоками, которые иначе слиплись бы, стоит VSpace.
    // Держим его здесь, а не при выводе: вставленная при выводе пустая строка
    // при следующем чтении стала бы блоком VSpace, которого в исходном IR не
    // было, и круг разошёлся бы.
    //
    // Такое случается на стыках, где наш канон длиннее исходника: незакрытый
    // забор дописывается закрывающим, и два блока кода, стоявшие вплотную,
    // разъезжаются.
    Document fixed;
    fixed.reserve(out.size() + 2);
    for (Block& blk : out) {
        if (!fixed.empty() && wouldMerge(fixed.back(), blk)) {
            Block gap;
            gap.kind = Kind::VSpace;
            fixed.push_back(std::move(gap));
        }
        fixed.push_back(std::move(blk));
    }
    c.doc = std::move(fixed);
}

}  // namespace

Document parse(std::string_view markdown) {
    Ctx c;
    c.buf = markdown.data();
    c.len = markdown.size();

    MD_PARSER parser{};
    parser.abi_version = 0;
    // Голые ссылки (https://…, www.…, почта) — часть диалекта GitHub, и в
    // заметках они встречаются куда чаще, чем размеченные вручную.
    parser.flags = MD_FLAG_TABLES | MD_FLAG_STRIKETHROUGH | MD_FLAG_TASKLISTS |
                   MD_FLAG_PERMISSIVEAUTOLINKS;
    parser.enter_block = enterBlock;
    parser.leave_block = leaveBlock;
    parser.enter_span = enterSpan;
    parser.leave_span = leaveSpan;
    parser.text = onText;
    parser.debug_log = nullptr;
    parser.syntax = nullptr;

    if (c.len == 0) return {};

    md_parse(c.buf, static_cast<MD_SIZE>(c.len), &parser, &c);

    endLeaf(c);
    finishExtents(c);
    return std::move(c.doc);
}

}  // namespace zametti
