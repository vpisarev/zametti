// IR → markdown.
//
// Вывод всегда одинаковый: '-' для маркированных списков, нумерация с 1,
// ATX-заголовки, огороженный код, strong снаружи emphasis. Подчёркивания нет.
//
// Отступ вложенных списков считается не «2 пробела на уровень», а по ширине
// маркера родителя. Для маркированных списков это те же 2 пробела, но под
// "1. " содержимое начинается с колонки 3, и вложенный список с отступом 2
// разорвал бы родительский — CommonMark считает такую строку не продолжением
// элемента, а новым блоком.

#include "serializer.h"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cctype>
#include <cstring>
#include <string>
#include <vector>

namespace zametti {
namespace {

bool isAsciiSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f'; }

bool isAsciiPunct(char c) {
    return (c >= '!' && c <= '/') || (c >= ':' && c <= '@') || (c >= '[' && c <= '`') ||
           (c >= '{' && c <= '~');
}

bool isDigit(char c) { return c >= '0' && c <= '9'; }

// Классы символов для правил «фланкирования» из CommonMark. Не-ASCII считаем
// обычной буквой: для кириллицы это верно, а редкие случаи юникодной пунктуации
// приведут лишь к одному лишнему обратному слэшу, а не к потере смысла.
bool flankWhitespace(const std::string& s, size_t i, bool atEdge) {
    if (atEdge) return true;
    return isAsciiSpace(s[i]);
}

bool flankPunct(const std::string& s, size_t i, bool atEdge) {
    if (atEdge) return false;
    return isAsciiPunct(s[i]);
}

// Слово с точки зрения markdown: то, что не пробел и не ASCII-пунктуация.
// Не-ASCII байты сюда попадают целиком — для кириллицы это верно.
bool isWordByte(char c) { return !isAsciiSpace(c) && !isAsciiPunct(c); }

// Может ли прогон из delim открыть или закрыть выделение в этом месте.
bool runCanDelimit(const std::string& s, size_t begin, size_t end) {
    bool prevEdge = (begin == 0);
    bool nextEdge = (end >= s.size());
    size_t prev = prevEdge ? 0 : begin - 1;
    size_t next = nextEdge ? 0 : end;

    // Начало и конец строки внутри текста тоже считаем «пробелом».
    if (!prevEdge && s[prev] == '\n') prevEdge = true;
    if (!nextEdge && s[next] == '\n') nextEdge = true;

    bool prevWs = flankWhitespace(s, prev, prevEdge);
    bool nextWs = flankWhitespace(s, next, nextEdge);
    bool prevPu = flankPunct(s, prev, prevEdge);
    bool nextPu = flankPunct(s, next, nextEdge);

    bool leftFlanking = !nextWs && (!nextPu || prevWs || prevPu);
    bool rightFlanking = !prevWs && (!prevPu || nextWs || nextPu);

    // У '_' правила строже: внутри слова он выделения не образует. Без этого
    // уточнения каждый snake_case в заметках обрастал бы слэшами.
    if (s[begin] == '_') {
        bool canOpen = leftFlanking && (!rightFlanking || prevPu);
        bool canClose = rightFlanking && (!leftFlanking || nextPu);
        return canOpen || canClose;
    }
    return leftFlanking || rightFlanking;
}

bool looksLikeEntity(const std::string& s, size_t i) {
    size_t j = i + 1;
    if (j < s.size() && s[j] == '#') {
        ++j;
        if (j < s.size() && (s[j] == 'x' || s[j] == 'X')) ++j;
    }
    size_t digitsBegin = j;
    while (j < s.size() && ((s[j] >= 'a' && s[j] <= 'z') || (s[j] >= 'A' && s[j] <= 'Z') ||
                            isDigit(s[j])))
        ++j;
    return j > digitsBegin && j < s.size() && s[j] == ';';
}

bool startsHtmlish(const std::string& s, size_t i) {
    size_t j = i + 1;
    if (j >= s.size()) return false;
    char c = s[j];
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '/' || c == '!' || c == '?';
}

// Скобку надо экранировать только если она действительно открывает ссылку:
// "[x]" сам по себе — обычный текст, ссылкой он станет лишь при определении
// вида "[x]: /a", а таких мы не выводим. Экранировать всё подряд нельзя:
// "1. [x] текст" обязано вернуться байт в байт.
bool bracketOpensLink(const std::string& s, size_t i) {
    int depth = 0;
    for (size_t j = i; j < s.size(); ++j) {
        if (s[j] == '\\') { ++j; continue; }
        if (s[j] == '\n') return false;
        if (s[j] == '[') ++depth;
        else if (s[j] == ']') {
            if (--depth == 0) return j + 1 < s.size() && s[j + 1] == '(';
        }
    }
    return false;
}

// Просто парная скобка, без разбора того, во что она превратится.
bool bracketPairAt(const std::string& s, size_t i) {
    int depth = 0;
    for (size_t j = i; j < s.size(); ++j) {
        if (s[j] == '\\') { ++j; continue; }
        if (s[j] == '\n') return false;
        if (s[j] == '[') ++depth;
        else if (s[j] == ']' && --depth == 0) return true;
    }
    return false;
}

size_t lineEndFrom(const std::string& s, size_t i) {
    size_t e = s.find('\n', i);
    return e == std::string::npos ? s.size() : e;
}

// Начало строки: конструкции, которые захватывают всю строку и потому меняют
// разбор, если их не экранировать.
bool needsLineStartEscape(const std::string& s, size_t i, size_t& escapeAt) {
    size_t e = lineEndFrom(s, i);
    char c = s[i];

    if (c == '>') { escapeAt = i; return true; }

    if (c == '#') {
        size_t j = i;
        while (j < e && s[j] == '#') ++j;
        if (j - i <= 6 && (j == e || s[j] == ' ' || s[j] == '\t')) { escapeAt = i; return true; }
        return false;
    }

    if (c == '-' || c == '+' || c == '*') {
        if (i + 1 == e || s[i + 1] == ' ' || s[i + 1] == '\t') { escapeAt = i; return true; }
    }

    if (isDigit(c)) {
        size_t j = i;
        while (j < e && isDigit(s[j])) ++j;
        if (j - i <= 9 && j < e && (s[j] == '.' || s[j] == ')') &&
            (j + 1 == e || s[j + 1] == ' ' || s[j + 1] == '\t')) {
            escapeAt = j;
            return true;
        }
    }

    // "[метка]: /url" в начале строки — определение ссылки, то есть целый блок,
    // а не текст. Внутри строки та же последовательность безобидна.
    if (c == '[') {
        size_t j = i + 1;
        while (j < e && s[j] != ']') {
            if (s[j] == '\\') ++j;
            ++j;
        }
        if (j + 1 < e && s[j] == ']' && s[j + 1] == ':') { escapeAt = i; return true; }
    }

    // Три и больше тильд в начале строки — открывающий забор блока кода.
    // Обычные правила фланкирования этого не ловят: у строки из одних тильд
    // с обеих сторон «пробел», и экранировать её вроде бы не за что.
    if (c == '~' || c == '`') {
        size_t j = i;
        while (j < e && s[j] == c) ++j;
        if (j - i >= 3) { escapeAt = i; return true; }
    }

    // Строка целиком из '=' или из '-'/'_'/'*' — setext-подчёркивание или
    // тематический разделитель.
    if (c == '=' || c == '-' || c == '_' || c == '*') {
        bool uniform = true;
        size_t cnt = 0;
        for (size_t j = i; j < e; ++j) {
            if (s[j] == c) { ++cnt; continue; }
            if (s[j] == ' ' || s[j] == '\t' || s[j] == '\r') continue;
            uniform = false;
            break;
        }
        if (uniform && cnt > 0) { escapeAt = i; return true; }
    }
    return false;
}

// Пометки по байтам текста, которые нельзя вывести из самого текста: они
// зависят от того, какие ограничители сериализатор поставит вокруг.
enum Mark : unsigned char {
    kMarkDelimEdge = 1,   // край выделения: '*'/'_'/'~' здесь слипнется с ограничителем
    kMarkInLink    = 2,   // текст ссылки: скобки внутри разорвут её
};

struct TextSink {
    std::string out;
    std::string contIndent;   // отступ строк-продолжений
    bool bol = true;          // стоим в начале строки (после маркера/отступа)
    const std::vector<unsigned char>* marks = nullptr;
    bool hasLinkDefs = false; // в документе есть "[x]: /url" — значит любая пара
                              // скобок может внезапно стать ссылкой
};

unsigned char markAt(const TextSink& sink, size_t i) {
    return (sink.marks != nullptr && i < sink.marks->size()) ? (*sink.marks)[i] : 0;
}

// Кладёт [begin, end) текста в вывод, экранируя ровно то, что иначе изменит
// разбор. Контекст (соседние символы) берётся из полного текста, чтобы разрез
// на прогоны стилей не влиял на решения.
void appendEscaped(TextSink& sink, const std::string& text, size_t begin, size_t end) {
    size_t i = begin;
    while (i < end) {
        char c = text[i];

        if (c == '\n') {
            sink.out.push_back('\n');
            sink.out += sink.contIndent;
            sink.bol = true;
            ++i;
            continue;
        }

        if (sink.bol) {
            size_t at = 0;
            if (needsLineStartEscape(text, i, at) && at < end) {
                sink.out.append(text, i, at - i);
                sink.out.push_back('\\');
                sink.out.push_back(text[at]);
                i = at + 1;
                sink.bol = false;
                continue;
            }
            sink.bol = false;
        }

        switch (c) {
            case '\\':
                sink.out += "\\\\";
                ++i;
                continue;
            case '`':
                sink.out += "\\`";
                ++i;
                continue;
            case '[':
                if (bracketOpensLink(text, i) || (markAt(sink, i) & kMarkInLink) != 0 ||
                    (sink.hasLinkDefs && bracketPairAt(text, i)))
                    sink.out.push_back('\\');
                sink.out.push_back('[');
                ++i;
                continue;
            case ']':
                if ((markAt(sink, i) & kMarkInLink) != 0) sink.out.push_back('\\');
                sink.out.push_back(']');
                ++i;
                continue;
            case '<':
                if (startsHtmlish(text, i)) sink.out.push_back('\\');
                sink.out.push_back('<');
                ++i;
                continue;
            case '&':
                if (looksLikeEntity(text, i)) sink.out.push_back('\\');
                sink.out.push_back('&');
                ++i;
                continue;
            case '*':
            case '_':
            case '~': {
                size_t j = i;
                while (j < text.size() && text[j] == c) ++j;
                bool escape = runCanDelimit(text, i, j);
                for (size_t k = i; k < j && !escape; ++k)
                    if ((markAt(sink, k) & kMarkDelimEdge) != 0) escape = true;
                size_t stop = j < end ? j : end;
                for (size_t k = i; k < stop; ++k) {
                    if (escape) sink.out.push_back('\\');
                    sink.out.push_back(c);
                }
                i = stop;
                continue;
            }
            default:
                sink.out.push_back(c);
                ++i;
                continue;
        }
    }
}

// Канон вложенности: ~~ снаружи, затем **, затем курсив, ссылка внутри всего.
//
// Курсив по канону — '_', но '_' внутри слова выделения не образует, поэтому
// на стыке со словом приходится брать '*'. Иначе "ksize*ksize" из вставленного
// кода после одного круга превратился бы в "ksize_ksize" и перестал разбираться
// как выделение — неподвижной точки не будет.
const char* italicDelim(const std::string& text, size_t begin, size_t end) {
    bool leftIntraword = begin > 0 && isWordByte(text[begin - 1]);
    bool rightIntraword = end < text.size() && isWordByte(text[end]);
    return (leftIntraword || rightIntraword) ? "*" : "_";
}

bool hrefNeedsBrackets(const std::string& h) {
    for (char c : h)
        if (isAsciiSpace(c) || c == '(' || c == ')' || c == '<' || c == '>' ||
            static_cast<unsigned char>(c) < 0x20)
            return true;
    return h.empty();
}

bool looksLikeEmail(const std::string& text) {
    size_t at = text.find('@');
    return at != std::string::npos && at > 0 && at + 1 < text.size() &&
           text.find('@', at + 1) == std::string::npos && text.find('.', at) != std::string::npos;
}

bool hasScheme(const std::string& text, bool requireSlashes) {
    size_t colon = text.find(':');
    if (colon == std::string::npos || colon == 0) return false;
    for (size_t i = 0; i < colon; ++i) {
        char c = text[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || isDigit(c) || c == '+' ||
                  c == '-' || c == '.';
        if (!ok) return false;
    }
    return !requireSlashes || text.compare(colon, 3, "://") == 0;
}

// Как выводить ссылку, у которой текст и есть адрес.
enum class LinkShape {
    Inline,   // [текст](адрес)
    Angle,    // <адрес>
    Bare,     // адрес как есть — разбор опознает его сам
};

// Годится ли адрес для вывода без разметки. Разбор подхватывает голую ссылку
// только пока в ней нет знаков, которые её оборвут или которые пришлось бы
// экранировать, — а экранировать внутри адреса нельзя, он от этого развалится.
bool bareSafe(const std::string& text) {
    if (text.empty()) return false;
    for (unsigned char c : text) {
        // Не-ASCII в голой ссылке разбор не принимает: "https://x/путь" без
        // угловых скобок остаётся обычным текстом.
        if (c <= ' ' || c >= 0x80) return false;
        if (std::strchr("`*_~\\[]<>&()\"'", c) != nullptr) return false;
    }
    // Хвостовую пунктуацию разбор в ссылку не включает, и голый вывод потерял бы
    // её из адреса.
    return std::strchr(".,;:!?", text.back()) == nullptr;
}

// Голой ссылкой разбор считает только адрес с настоящим доменом: "https://../"
// в угловых скобках ссылка, а без них — обычный текст.
bool hasRealHost(const std::string& text) {
    size_t begin = text.find("://");
    if (begin == std::string::npos) return false;
    begin += 3;
    size_t end = text.find_first_of("/?#", begin);
    if (end == std::string::npos) end = text.size();
    if (begin >= end) return false;
    if (!std::isalnum(static_cast<unsigned char>(text[begin]))) return false;
    for (size_t i = begin; i + 1 < end; ++i)
        if (text[i] == '.' && std::isalnum(static_cast<unsigned char>(text[i + 1]))) return true;
    return false;
}

// Голые ссылки разбор опознаёт и без разметки, поэтому и выводить их надо
// голыми: обернув "https://x" в угловые скобки, мы переписали бы каждую заметку,
// где ссылка просто набрана в строку. Но опознаёт он не всё подряд — только три
// схемы, "www." и почту, поэтому список здесь закрытый, а не "любая схема".
LinkShape linkShape(const std::string& text, const std::string& href) {
    if (text.empty()) return LinkShape::Inline;
    for (char c : text)
        if (isAsciiSpace(c) || c == '<' || c == '>') return LinkShape::Inline;

    if (href == text) {
        const bool bareScheme = text.compare(0, 7, "http://") == 0 ||
                                text.compare(0, 8, "https://") == 0 ||
                                text.compare(0, 6, "ftp://") == 0;
        if (bareScheme && bareSafe(text) && hasRealHost(text)) return LinkShape::Bare;
        if (hasScheme(text, /*requireSlashes=*/false)) return LinkShape::Angle;
        return LinkShape::Inline;
    }

    if (href.size() == text.size() + 7 && href.compare(0, 7, "mailto:") == 0 &&
        href.compare(7, std::string::npos, text) == 0 && looksLikeEmail(text) &&
        bareSafe(text)) {
        return LinkShape::Bare;
    }

    if (href.size() == text.size() + 7 && href.compare(0, 7, "http://") == 0 &&
        href.compare(7, std::string::npos, text) == 0 && text.compare(0, 4, "www.") == 0 &&
        bareSafe(text)) {
        return LinkShape::Bare;
    }
    return LinkShape::Inline;
}

void appendHref(std::string& out, const std::string& href) {
    if (!hrefNeedsBrackets(href)) {
        out += href;
        return;
    }
    out.push_back('<');
    for (char c : href) {
        if (c == '<' || c == '>' || c == '\\') out.push_back('\\');
        out.push_back(c);
    }
    out.push_back('>');
}

void appendCodeSpan(std::string& out, const std::string& content) {
    size_t longest = 0;
    size_t run = 0;
    for (char c : content) {
        run = (c == '`') ? run + 1 : 0;
        if (run > longest) longest = run;
    }
    std::string ticks(longest + 1, '`');

    bool pad = !content.empty() &&
               (content.front() == '`' || content.back() == '`' ||
                (isAsciiSpace(content.front()) && isAsciiSpace(content.back()) &&
                 content.find_first_not_of(" \t\n") != std::string::npos));

    out += ticks;
    if (pad) out.push_back(' ');
    out += content;
    if (pad) out.push_back(' ');
    out += ticks;
}

// Модель плоская: спаны идут подряд и не вкладываются друг в друга. Выводить их
// так же плоско нельзя — соседние ограничители склеиваются. "*a*" и следом
// "**b**" дадут "*a***b**", а это уже не то, что было. Поэтому перед выводом
// вложенность восстанавливается: у соседних спанов ищется общий признак, и он
// выносится наружу одним ограничителем.
// Порядок — канонический: ~~ снаружи, затем **, затем курсив, затем ссылка.
// Встроенный код всегда внутри всего: разметки внутри него нет по определению.
enum Attr { kStrike = 0, kBold = 1, kItalic = 2, kHref = 3, kCode = 4, kAttrCount = 5 };

struct Segment {
    size_t begin = 0;
    size_t end = 0;
    const Span* span = nullptr;   // nullptr → голый текст между спанами
};

bool hasAttr(const Span* s, int a) {
    if (s == nullptr) return false;
    switch (a) {
        case kStrike: return s->strike;
        case kBold:   return s->bold;
        case kItalic: return s->italic;
        case kHref:   return !s->href.empty();
        case kCode:   return s->code;
        default:      return false;
    }
}

std::vector<Segment> splitIntoSegments(const Block& b) {
    std::vector<Segment> segs;
    size_t pos = 0;
    for (const Span& s : b.inlines) {
        size_t so = static_cast<size_t>(s.offset);
        size_t se = so + static_cast<size_t>(s.length);
        if (so > b.text.size() || se > b.text.size() || se < so || so < pos) continue;
        if (so > pos) segs.push_back(Segment{pos, so, nullptr});
        segs.push_back(Segment{so, se, &s});
        pos = se;
    }
    if (pos < b.text.size()) segs.push_back(Segment{pos, b.text.size(), nullptr});
    return segs;
}

// Длина ряда соседей, у которых есть этот же признак.
size_t runLength(const std::vector<Segment>& segs, size_t i, size_t hi, int attr) {
    const std::string& href = segs[i].span->href;
    size_t j = i;
    while (j < hi && hasAttr(segs[j].span, attr) && (attr != kHref || segs[j].span->href == href))
        ++j;
    return j - i;
}

void emitSegments(TextSink& sink, const std::string& text, const std::vector<Segment>& segs,
                  size_t lo, size_t hi, unsigned openMask,
                  std::vector<unsigned char>* marksBuf) {
    size_t i = lo;
    while (i < hi) {
        // Наружу выносится признак, покрывающий самый длинный ряд: "***foo** bar*"
        // — это курсив, внутри которого жирный кусок, а не наоборот. При равной
        // длине выигрывает канонический порядок: ~~ снаружи, затем **, затем _.
        int attr = -1;
        size_t best = 0;
        for (int a = 0; a < kAttrCount; ++a) {
            if ((openMask & (1u << a)) != 0) continue;
            if (!hasAttr(segs[i].span, a)) continue;
            size_t len = runLength(segs, i, hi, a);
            if (len > best) { best = len; attr = a; }
        }
        if (attr < 0) {
            appendEscaped(sink, text, segs[i].begin, segs[i].end);
            ++i;
            continue;
        }

        const std::string& href = segs[i].span->href;
        size_t j = i + best;

        size_t gb = segs[i].begin;
        size_t ge = segs[j - 1].end;

        // Содержимое встроенного кода буквально: ни экранирования, ни вложенной
        // разметки. Длина ограничителя подбирается так, чтобы он не встретился
        // внутри, а пробелы-подкладки нужны, когда содержимое само начинается
        // или кончается кавычкой.
        if (attr == kCode) {
            appendCodeSpan(sink.out, text.substr(gb, ge - gb));
            sink.bol = false;
            i = j;
            continue;
        }

        // Адрес выводится дословно, без экранирования: подчёркивания и тильды
        // внутри URL экранировать нельзя, иначе ссылка развалится.
        if (attr == kHref && openMask == 0 && j == i + 1) {
            const LinkShape shape = linkShape(text.substr(gb, ge - gb), href);
            if (shape != LinkShape::Inline) {
                if (shape == LinkShape::Angle) sink.out.push_back('<');
                sink.out.append(text, gb, ge - gb);
                if (shape == LinkShape::Angle) sink.out.push_back('>');
                sink.bol = false;
                i = j;
                continue;
            }
        }

        // Пометить края: символ-ограничитель, оказавшийся у самой границы
        // выделения, слипнется с ним ("foo **\***" превратилось бы в "foo ***").
        // Скобки внутри текста ссылки её разрывают, поэтому весь диапазон
        // помечается целиком.
        if (marksBuf != nullptr && ge > gb) {
            if (attr == kHref) {
                for (size_t k = gb; k < ge; ++k) (*marksBuf)[k] |= kMarkInLink;
            } else {
                (*marksBuf)[gb] |= kMarkDelimEdge;
                (*marksBuf)[ge - 1] |= kMarkDelimEdge;
            }
        }

        const char* italic = italicDelim(text, gb, ge);
        switch (attr) {
            case kStrike: sink.out += "~~"; break;
            case kBold:   sink.out += "**"; break;
            case kItalic: sink.out += italic; break;
            case kHref:   sink.out.push_back('['); break;
            default: break;
        }
        sink.bol = false;

        emitSegments(sink, text, segs, i, j, openMask | (1u << attr), marksBuf);

        switch (attr) {
            case kStrike: sink.out += "~~"; break;
            case kBold:   sink.out += "**"; break;
            case kItalic: sink.out += italic; break;
            case kHref:
                sink.out += "](";
                appendHref(sink.out, href);
                sink.out.push_back(')');
                break;
            default: break;
        }
        i = j;
    }
}

void appendInlineText(TextSink& sink, const Block& b) {
    std::vector<Segment> segs = splitIntoSegments(b);

    // Пометки зависят от расстановки ограничителей, а она — от текста. Поэтому
    // первый проход только собирает пометки, а выводит уже второй.
    std::vector<unsigned char> marks(b.text.size(), 0);
    TextSink probe;
    probe.contIndent = sink.contIndent;
    probe.bol = sink.bol;
    probe.hasLinkDefs = sink.hasLinkDefs;
    emitSegments(probe, b.text, segs, 0, segs.size(), 0, &marks);

    sink.marks = &marks;
    emitSegments(sink, b.text, segs, 0, segs.size(), 0, nullptr);
    sink.marks = nullptr;
}

// Забор должен быть длиннее самого длинного прогона того же символа в строках
// содержимого, иначе содержимое закроет блок само. Обратные кавычки в
// info-строке для них запрещены — тогда забор из тильд.
std::string fenceFor(const std::string& code, const std::string& info) {
    char ch = (info.find('`') != std::string::npos) ? '~' : '`';
    size_t longest = 0;
    size_t run = 0;
    bool atBol = true;
    for (char c : code) {
        if (c == ch && atBol) {
            ++run;
            if (run > longest) longest = run;
            continue;
        }
        run = 0;
        atBol = (c == '\n');
    }
    size_t n = longest >= 3 ? longest + 1 : 3;
    return std::string(n, ch);
}

void validate(const Block& b) {
    if (!b.rawSource.empty()) return;
    assert((b.kind == Kind::Heading) == (b.headingLevel != 0) &&
           "headingLevel осмыслен только у заголовка");
    assert((b.kind != Kind::Heading || (b.headingLevel >= 1 && b.headingLevel <= 6)) &&
           "уровень заголовка вне 1..6");
    assert((isList(b.kind) || b.level == 0) && "level осмыслен только у элементов списка");
    assert((b.kind == Kind::Code || b.info.empty()) && "info осмыслена только у блока кода");
    assert(b.level >= 0 && "отрицательный уровень вложенности");
}

std::string markerFor(Kind kind, int ordinal) {
    switch (kind) {
        case Kind::Bullet:        return "- ";
        case Kind::TaskUnchecked: return "- [ ] ";
        case Kind::TaskChecked:   return "- [x] ";
        case Kind::Ordered: {
            char buf[24];
            std::snprintf(buf, sizeof(buf), "%d. ", ordinal);
            return buf;
        }
        default:
            return {};
    }
}

// Ширина собственно маркера списка. Чекбокс "[ ] " маркером не является — это
// уже содержимое элемента, и вложенный список отсчитывается не от него:
// "- [ ] a" + "  - b" даёт вложенность, а не продолжение текста.
size_t markerIndentWidth(Kind kind, int ordinal) {
    if (kind != Kind::Ordered) return 2;
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%d. ", ordinal);
    return std::string(buf).size();
}

// Определение ссылки в дословном куске: "[метка]: /url". Если такое в документе
// есть, то любая пара скобок в тексте может при разборе стать ссылкой, и её
// приходится экранировать.
bool looksLikeLinkDefinition(const std::string& raw) {
    size_t i = 0;
    while (i < raw.size()) {
        size_t e = raw.find('\n', i);
        if (e == std::string::npos) e = raw.size();
        size_t p = i;
        size_t indent = 0;
        while (p < e && raw[p] == ' ' && indent < 4) { ++p; ++indent; }
        if (p < e && raw[p] == '[' && indent < 4) {
            size_t q = p + 1;
            while (q < e && raw[q] != ']') {
                if (raw[q] == '\\') ++q;
                ++q;
            }
            if (q + 1 < e && raw[q] == ']' && raw[q + 1] == ':') return true;
        }
        i = e + 1;
    }
    return false;
}

}  // namespace

std::string serialize(const Document& doc) {
    std::string out;

    bool hasLinkDefs = false;
    for (const Block& b : doc)
        if (!b.rawSource.empty() && looksLikeLinkDefinition(b.rawSource)) { hasLinkDefs = true; break; }

    // Колонка, с которой начинается содержимое на каждом уровне вложенности,
    // и счётчики нумерации.
    std::vector<size_t> contentCol{0};
    std::vector<int> ordinal{0};
    std::vector<char> runAlive{0};      // на этом уровне прогон ещё идёт
    std::vector<char> runOrdered{0};    // и он нумерованный

    bool prevWasList = false;
    bool prevWasQuote = false;
    // Нужен только ассерту ниже: в сборке с NDEBUG он исчезает вместе с ним.
    [[maybe_unused]] int prevLevel = -1;

    for (size_t i = 0; i < doc.size(); ++i) {
        const Block& b = doc[i];
        validate(b);

        bool thisIsList = b.rawSource.empty() && isList(b.kind);
        bool thisIsQuote = b.rawSource.empty() && b.kind == Kind::Quote;

        if (i > 0) {
            // Пункты одного списка не разделяются пустой строкой, а абзацы
            // одной цитаты — строкой ">": иначе цитата развалилась бы на две.
            if (prevWasList && thisIsList) {}
            else if (prevWasQuote && thisIsQuote) out += ">\n";
            else out += "\n";
        }

        if (!b.rawSource.empty()) {
            out += b.rawSource;
            if (out.empty() || out.back() != '\n') out.push_back('\n');
            std::fill(runAlive.begin(), runAlive.end(), 0);
            prevWasList = false;
            prevWasQuote = false;
            prevLevel = -1;
            continue;
        }

        switch (b.kind) {
            case Kind::Heading: {
                out.append(static_cast<size_t>(b.headingLevel), '#');
                TextSink sink;
                sink.bol = false;
                sink.hasLinkDefs = hasLinkDefs;
                appendInlineText(sink, b);
                if (!sink.out.empty()) {
                    out.push_back(' ');
                    // Хвостовой прогон '#' Markdown считает закрывающей
                    // последовательностью и выбрасывает.
                    size_t hashes = 0;
                    while (hashes < sink.out.size() &&
                           sink.out[sink.out.size() - 1 - hashes] == '#')
                        ++hashes;
                    if (hashes > 0) sink.out.insert(sink.out.size() - hashes, "\\");
                    out += sink.out;
                }
                out.push_back('\n');
                break;
            }

            case Kind::Code: {
                std::string fence = fenceFor(b.text, b.info);
                out += fence;
                out += b.info;
                out.push_back('\n');
                out += b.text;
                if (!b.text.empty() && b.text.back() != '\n') out.push_back('\n');
                out += fence;
                out.push_back('\n');
                break;
            }

            case Kind::Paragraph: {
                TextSink sink;
                sink.hasLinkDefs = hasLinkDefs;
                appendInlineText(sink, b);
                out += sink.out;
                out.push_back('\n');
                break;
            }

            case Kind::Quote: {
                TextSink sink;
                sink.contIndent = "> ";
                sink.hasLinkDefs = hasLinkDefs;
                appendInlineText(sink, b);
                if (sink.out.empty()) {
                    out.push_back('>');
                } else {
                    out += "> ";
                    out += sink.out;
                }
                out.push_back('\n');
                break;
            }

            default: {   // элементы списка
                assert(b.level <= prevLevel + 1 &&
                       "уровень вложенности перепрыгнут: такого разбор не порождает");
                size_t level = static_cast<size_t>(b.level);
                if (contentCol.size() <= level + 1) contentCol.resize(level + 2, 0);
                if (ordinal.size() <= level) {
                    ordinal.resize(level + 1, 0);
                    runAlive.resize(level + 1, 0);
                    runOrdered.resize(level + 1, 0);
                }

                // Прогон на уровне продолжается и через вложенный подсписок:
                // "1. / 1.1 / 2." — второй пункт верхнего уровня всё ещё второй.
                bool ord = isOrdered(b.kind);
                bool sameRun = runAlive[level] && (runOrdered[level] != 0) == ord;
                ordinal[level] = sameRun ? ordinal[level] + 1 : 1;
                runAlive[level] = 1;
                runOrdered[level] = ord ? 1 : 0;
                for (size_t k = level + 1; k < ordinal.size(); ++k) runAlive[k] = 0;

                std::string marker = markerFor(b.kind, ordinal[level]);
                size_t indent = contentCol[level];
                size_t childIndent = indent + markerIndentWidth(b.kind, ordinal[level]);
                contentCol[level + 1] = childIndent;

                out.append(indent, ' ');
                if (b.text.empty()) {
                    // "- " с висящим пробелом Markdown бы съел, но глазами это
                    // читается как мусор.
                    while (!marker.empty() && marker.back() == ' ') marker.pop_back();
                    out += marker;
                } else {
                    out += marker;
                    TextSink sink;
                    sink.contIndent = std::string(childIndent, ' ');
                    sink.hasLinkDefs = hasLinkDefs;
                    appendInlineText(sink, b);
                    out += sink.out;
                }
                out.push_back('\n');
                break;
            }
        }

        if (!thisIsList) std::fill(runAlive.begin(), runAlive.end(), 0);
        prevWasList = thisIsList;
        prevWasQuote = thisIsQuote;
        prevLevel = thisIsList ? b.level : -1;
    }

    return out;
}

}  // namespace zametti
