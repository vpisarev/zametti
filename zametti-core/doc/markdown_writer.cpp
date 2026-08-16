// ZDocument → канонический markdown. Часть класса, а не отдельная библиотека:
// свободной функции «сериализовать документ» больше нет вовсе, и это нарочно —
// пока она была, все ходили мимо класса.
//
// КАНОН. Zametti не ставит целью сохранить чужой markdown байт в байт: при
// первом открытии он приводится к одному виду — заголовки только ATX, буллет
// «-», нумерация настоящими номерами, курсив «_», хвостовые пробелы прочь,
// файл кончается одним переводом строки. Отсюда главный инвариант: причесали
// один раз — дальше ни байта.

#include "document_impl.h"

#include "document_pieces.h"

#include "block_kind.h"
#include "doc_model.h"
#include "document_builder.h"
#include "math_scan.h"

#include <QString>
#include <QTextBlock>
#include <QTextCharFormat>
#include <QTextDocument>
#include <QTextFragment>

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace zametti {
namespace {

// Слипнутся ли два блока, окажись они в файле подряд без пустой строки.
bool wouldMerge(const Piece& previous, const Piece& next) {
    if (previous.isClosedHtmlComment()) return false;
    if (!previous.raw && previous.kind == Kind::Html) return false;
    // Два блока кода подряд: их заборы спарились бы не так, как надо. Два
    // дословных куска подряд — по той же причине непрозрачности.
    const bool prevLiteral = previous.raw || previous.kind == Kind::Code;
    const bool nextLiteral = next.raw || next.kind == Kind::Code;
    if (prevLiteral && nextLiteral) return true;
    // Разделитель ("___") стоит вплотную к любому соседу: подчёркивание не
    // бывает setext-подчёркиванием. Ровно ради этого канон его и выбрал.
    if (next.kind != Kind::Paragraph) return false;
    return previous.kind == Kind::Paragraph || previous.kind == Kind::Quote ||
           isList(previous.kind);
}

}  // namespace


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
bool flankWhitespace(std::string_view s, size_t i, bool atEdge) {
    if (atEdge) return true;
    return isAsciiSpace(s[i]);
}

bool flankPunct(std::string_view s, size_t i, bool atEdge) {
    if (atEdge) return false;
    return isAsciiPunct(s[i]);
}

// Слово с точки зрения markdown: то, что не пробел и не ASCII-пунктуация.
// Не-ASCII байты сюда попадают целиком — для кириллицы это верно.
bool isWordByte(char c) { return !isAsciiSpace(c) && !isAsciiPunct(c); }

// Может ли прогон из delim открыть или закрыть выделение в этом месте.
bool runCanDelimit(std::string_view s, size_t begin, size_t end) {
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

bool looksLikeEntity(std::string_view s, size_t i) {
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

bool startsHtmlish(std::string_view s, size_t i) {
    size_t j = i + 1;
    if (j >= s.size()) return false;
    char c = s[j];
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '/' || c == '!' || c == '?';
}

// Скобку надо экранировать только если она действительно открывает ссылку:
// "[x]" сам по себе — обычный текст, ссылкой он станет лишь при определении
// вида "[x]: /a", а таких мы не выводим. Экранировать всё подряд нельзя:
// "1. [x] текст" обязано вернуться байт в байт.
bool bracketOpensLink(std::string_view s, size_t i) {
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
bool bracketPairAt(std::string_view s, size_t i) {
    int depth = 0;
    for (size_t j = i; j < s.size(); ++j) {
        if (s[j] == '\\') { ++j; continue; }
        if (s[j] == '\n') return false;
        if (s[j] == '[') ++depth;
        else if (s[j] == ']' && --depth == 0) return true;
    }
    return false;
}

size_t lineEndFrom(std::string_view s, size_t i) {
    size_t e = s.find('\n', i);
    return e == std::string_view::npos ? s.size() : e;
}

// Начало строки: конструкции, которые захватывают всю строку и потому меняют
// разбор, если их не экранировать.
bool needsLineStartEscape(std::string_view s, size_t i, size_t& escapeAt) {
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
// Начал бы этот доллар формулу, если оставить его голым? Спрашиваем ОБЩИЙ
// канон (math_scan.h), а не гадаем: у сериализатора и у разбора правило одно,
// иначе они разойдутся молча.
bool dollarOpensMath(std::string_view text, size_t at) {
    for (const MathSpan& span : scanMath(text.substr(at)))
        return span.start == 0;   // первая найденная либо здесь, либо дальше
    return false;
}

void appendEscaped(TextSink& sink, std::string_view text, size_t begin, size_t end) {
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
            case '$':
                // ДОЛЛАР ЭКРАНИРУЕТСЯ, ТОЛЬКО ЕСЛИ ОН НАЧАЛ БЫ ФОРМУЛУ. Иначе
                // цены («заплатил $5») обросли бы косыми на ровном месте, а
                // это тот самый шум в файле, которого формат избегает. Но
                // молча отдать `\$x\$` обратно как `$x$` нельзя: при чтении
                // это станет математикой, и текст поменяет смысл.
                if (dollarOpensMath(text, i)) sink.out.push_back('\\');
                sink.out.push_back('$');
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
const char* italicDelim(std::string_view text, size_t begin, size_t end) {
    bool leftIntraword = begin > 0 && isWordByte(text[begin - 1]);
    bool rightIntraword = end < text.size() && isWordByte(text[end]);
    return (leftIntraword || rightIntraword) ? "*" : "_";
}

bool hrefNeedsBrackets(std::string_view h) {
    for (char c : h)
        if (isAsciiSpace(c) || c == '(' || c == ')' || c == '<' || c == '>' ||
            static_cast<unsigned char>(c) < 0x20)
            return true;
    return h.empty();
}

bool looksLikeEmail(std::string_view text) {
    size_t at = text.find('@');
    return at != std::string_view::npos && at > 0 && at + 1 < text.size() &&
           text.find('@', at + 1) == std::string_view::npos &&
           text.find('.', at) != std::string_view::npos;
}

bool hasScheme(std::string_view text, bool requireSlashes) {
    size_t colon = text.find(':');
    if (colon == std::string_view::npos || colon == 0) return false;
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
bool bareSafe(std::string_view text) {
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
bool hasRealHost(std::string_view text) {
    size_t begin = text.find("://");
    if (begin == std::string_view::npos) return false;
    begin += 3;
    size_t end = text.find_first_of("/?#", begin);
    if (end == std::string_view::npos) end = text.size();
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
LinkShape linkShape(std::string_view text, std::string_view href) {
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
        href.compare(7, std::string_view::npos, text) == 0 && looksLikeEmail(text) &&
        bareSafe(text)) {
        return LinkShape::Bare;
    }

    if (href.size() == text.size() + 7 && href.compare(0, 7, "http://") == 0 &&
        href.compare(7, std::string_view::npos, text) == 0 &&
        text.compare(0, 4, "www.") == 0 && bareSafe(text)) {
        return LinkShape::Bare;
    }
    return LinkShape::Inline;
}

void appendHref(std::string& out, std::string_view href) {
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

void appendCodeSpan(std::string& out, std::string_view content) {
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
                 content.find_first_not_of(" \t\n") != std::string_view::npos));

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
    const Run* span = nullptr;   // nullptr → голый текст между кусками
};

bool hasAttr(const Run* s, int a) {
    if (s == nullptr) return false;
    // Картинка, строчный комментарий и формула выводятся целиком отдельными
    // ветвями; в ряды соседних признаков их втягивать нельзя.
    if (s->image()) return false;
    if (s->comment()) return false;
    if (s->math()) return false;
    switch (a) {
        case kStrike: return s->strike();
        case kBold:   return s->bold();
        case kItalic: return s->italic();
        case kHref:   return !s->href.empty();
        case kCode:   return s->code();
        default:      return false;
    }
}

// Смещения спанов относительные — от начала текста блока, — поэтому сегменты
// индексируют ровно этот текст, и пересчитывать ничего не нужно.
std::vector<Segment> splitIntoSegments(const Piece& b) {
    const size_t textSize = b.text.size();
    std::vector<Segment> segs;
    size_t pos = 0;
    for (const Run& s : b.runs) {
        size_t so = static_cast<size_t>(s.start);
        size_t se = static_cast<size_t>(s.end);
        if (so > textSize || se > textSize || se < so || so < pos) continue;
        if (so > pos) segs.push_back(Segment{pos, so, nullptr});
        segs.push_back(Segment{so, se, &s});
        pos = se;
    }
    if (pos < textSize) segs.push_back(Segment{pos, textSize, nullptr});
    return segs;
}

// Длина ряда соседей, у которых есть этот же признак.
size_t runLength(const std::vector<Segment>& segs, size_t i, size_t hi, int attr) {
    const std::string_view href = segs[i].span->href;
    size_t j = i;
    while (j < hi && hasAttr(segs[j].span, attr) &&
           (attr != kHref || segs[j].span->href == href))
        ++j;
    return j - i;
}

void emitSegments(TextSink& sink, const Piece& b, std::string_view text,
                  const std::vector<Segment>& segs, size_t lo, size_t hi, unsigned openMask,
                  std::vector<unsigned char>* marksBuf) {
    size_t i = lo;
    while (i < hi) {
        // ФОРМУЛА — ДОСЛОВНО И ЦЕЛИКОМ, вместе с долларами. Ни одного
        // экранирования: внутри математики `\` это команда, `_` индекс, а
        // `\,` тонкий пробел. Именно здесь и терялось то, что портило файл до
        // этапа 16: общий путь текста писал `\\` вместо `\` и `,` вместо
        // `\,`.
        if (segs[i].span != nullptr && segs[i].span->math()) {
            sink.out.append(text, segs[i].begin, segs[i].end - segs[i].begin);
            sink.bol = false;
            ++i;
            continue;
        }

        // Строчный комментарий: внутренность буквальна, скобки — структура.
        // Канонические крайние пробелы, как у блочного.
        if (segs[i].span != nullptr && segs[i].span->comment()) {
            sink.out += "<!-- ";
            sink.out.append(text, segs[i].begin, segs[i].end - segs[i].begin);
            sink.out += " -->";
            sink.bol = false;
            ++i;
            continue;
        }

        // Картинка: подпись дословно-плоская по построению (разбор деградирует
        // иначе), поэтому весь спан выводится одним куском. Скобки в подписи
        // экранируются как в тексте ссылки — иначе подпись оборвётся.
        if (segs[i].span != nullptr && segs[i].span->image()) {
            const Run& img = *segs[i].span;
            if (marksBuf != nullptr)
                for (size_t k = segs[i].begin; k < segs[i].end; ++k)
                    (*marksBuf)[k] |= kMarkInLink;
            sink.out += "![";
            sink.bol = false;
            appendEscaped(sink, text, segs[i].begin, segs[i].end);
            sink.out += "](";
            appendHref(sink.out, img.href);
            const std::string_view imgTitle = img.title;
            if (!imgTitle.empty()) {
                // Кавычку и перевод строки разбор в title не пускает; обратная
                // косая экранируется, чтобы не съела закрывающую кавычку.
                sink.out += " \"";
                for (char tc : imgTitle) {
                    if (tc == '\\') sink.out.push_back('\\');
                    sink.out.push_back(tc);
                }
                sink.out.push_back('"');
            }
            sink.out.push_back(')');
            ++i;
            continue;
        }
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

        const std::string_view href = segs[i].span->href;
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

        emitSegments(sink, b, text, segs, i, j, openMask | (1u << attr), marksBuf);

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

void appendInlineText(TextSink& sink, const Piece& b) {
    const std::string_view text = b.text;
    std::vector<Segment> segs = splitIntoSegments(b);

    // Пометки зависят от расстановки ограничителей, а она — от текста. Поэтому
    // первый проход только собирает пометки, а выводит уже второй.
    std::vector<unsigned char> marks(text.size(), 0);
    TextSink probe;
    probe.contIndent = sink.contIndent;
    probe.bol = sink.bol;
    probe.hasLinkDefs = sink.hasLinkDefs;
    emitSegments(probe, b, text, segs, 0, segs.size(), 0, &marks);

    sink.marks = &marks;
    emitSegments(sink, b, text, segs, 0, segs.size(), 0, nullptr);
    sink.marks = nullptr;
}

// Забор должен быть длиннее самого длинного прогона того же символа в строках
// содержимого, иначе содержимое закроет блок само. Обратные кавычки в
// info-строке для них запрещены — тогда забор из тильд.
std::string fenceFor(std::string_view code, std::string_view info) {
    char ch = (info.find('`') != std::string_view::npos) ? '~' : '`';
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

void validate([[maybe_unused]] const Piece& b) {
    if (b.raw) return;
    assert((b.kind == Kind::Heading) == (b.headingLevel != 0) &&
           "headingLevel осмыслен только у заголовка");
    assert((b.kind != Kind::Heading || (b.headingLevel >= 1 && b.headingLevel <= 6)) &&
           "уровень заголовка вне 1..6");
    assert((!isList(b.kind) || b.level >= 0) && "у пункта списка уровень обязателен");
    // Внутри пункта могут стоять абзац, цитата и код — им уровень осмыслен.
    // Заголовку и пустой строке — нет: заголовка внутри пункта markdown не
    // выражает, а пустая строка ничьей вложенности не имеет.
    assert(((b.kind != Kind::Heading && b.kind != Kind::VSpace && b.kind != Kind::Divider) ||
            b.level == -1) &&
           "этому роду уровень не положен");
    assert((b.kind == Kind::ListItem || !b.checked) && "отметка осмысленна только у задачи");
    assert((b.kind == Kind::Code || b.info.empty()) && "info осмыслена только у блока кода");
    assert(b.level >= -1 && "уровень мельче, чем вне списка");
    assert((b.kind != Kind::Html || b.text.find("-->") == std::string::npos) &&
           "внутренность комментария не может содержать -->");
    assert((b.kind != Kind::Html || b.runs.empty()) &&
           "внутри комментария разметки не бывает");
    for ([[maybe_unused]] const Run& s : b.runs) {
        assert((!s.image() || !s.href.empty()) && "у картинки обязан быть путь");
        assert((!s.comment() ||
                (s.flags == InlineComment && s.href.empty())) &&
               "строчный комментарий не сочетается с другой разметкой");
        assert((!s.math() || (s.flags == InlineMath && s.href.empty())) &&
               "формула не сочетается с другой разметкой");
        assert((s.title.empty() || s.image()) && "title осмыслен только у картинки");
        assert((!s.image() ||
                (s.flags & (InlineBold | InlineItalic | InlineStrike | InlineCode)) == 0) &&
               "картинка не сочетается с другой разметкой");
    }
}

std::string markerFor(const Piece& b, int ordinal) {
    switch (b.marker) {
        case Marker::Bullet:  return "- ";
        case Marker::Task:    return b.checked ? "- [x] " : "- [ ] ";
        case Marker::Ordered: {
            char buf[24];
            std::snprintf(buf, sizeof(buf), "%d. ", ordinal);
            return buf;
        }
    }
    return {};
}

// Ширина собственно маркера списка. Чекбокс "[ ] " маркером не является — это
// уже содержимое элемента, и вложенный список отсчитывается не от него:
// "- [ ] a" + "  - b" даёт вложенность, а не продолжение текста.
size_t markerIndentWidth(const Piece& b, int ordinal) {
    if (b.marker != Marker::Ordered) return 2;
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%d. ", ordinal);
    return std::string(buf).size();
}

// Отступ блока, стоящего внутри пункта: колонка содержимого того пункта. Вне
// списка отступа нет. Колонки считает сам обход по пунктам, здесь мы их только
// читаем — и осторожно: у оторвавшегося блока колонки может и не оказаться.
size_t indentInsideItem(const Piece& b, const std::vector<size_t>& contentCol) {
    if (b.level < 0) return 0;
    const size_t at = static_cast<size_t>(b.level) + 1;
    return at < contentCol.size() ? contentCol[at] : 0;
}

// Определение ссылки в дословном куске: "[метка]: /url". Если такое в документе
// есть, то любая пара скобок в тексте может при разборе стать ссылкой, и её
// приходится экранировать.
bool looksLikeLinkDefinition(std::string_view raw) {
    size_t i = 0;
    while (i < raw.size()) {
        size_t e = raw.find('\n', i);
        if (e == std::string_view::npos) e = raw.size();
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

// Лишние неразрывные пробелы — обычными.
//
// Неразрывный пробел у нас ЗНАЧИМ, но только в НАЧАЛЕ строки: им держится
// отступ, потому что обычный пробел markdown в начале строки съедает (см.
// withEdgesNormalised). Везде дальше он не значит ничего — и приезжает мусором
// из чужих выгрузок: в одной заметке владельца их 437, из них 317 стоят прямо
// между словами, и даже внутри блоков кода.
//
// Правило выведено ЗАМЕРОМ по корпусу владельца (274 заметки): ведущих
// неразрывных 179, одиночных в середине строк 346, а серий из двух и более —
// НИ ОДНОЙ. Значит:
//
//   * ведущие не трогаем никогда — это наш отступ;
//   * ОДИНОЧНЫЙ в середине — мусор из чужой выгрузки, становится обычным;
//   * СЕРИЯ из двух и более в середине — выравнивание, и его мы теперь пишем
//     сами: столбик "int a     = 5" из блока кода, превращённого в абзац,
//     держится только неразрывными (обычные markdown схлопнет). Трогать её
//     значило бы ломать то, что сами и поставили.
std::string normaliseSpaces(std::string_view text) {
    static const std::string nbsp = "\xC2\xA0";
    std::string out;
    out.reserve(text.size());
    bool leading = true;    // мы всё ещё в отступе строки
    bool inCode = false;    // между заборами блока кода
    for (size_t i = 0; i < text.size();) {
        if (leading) {
            // Забор блока кода: три знака и больше, с любым отступом перед
            // ними. Внутри блока НЕРАЗРЫВНЫХ НЕ БЫВАЕТ ВОВСЕ — там значим сам
            // пробел, его копируют в терминал, а неразрывный туда попадает
            // только мусором из чужих выгрузок.
            size_t at = i;
            while (at < text.size() && (text[at] == ' ' || text[at] == '\t')) ++at;
            if (text.compare(at, 3, "```") == 0 || text.compare(at, 3, "~~~") == 0)
                inCode = !inCode;
        }
        if (text[i] == '\n') {
            out.push_back('\n');
            leading = true;
            ++i;
            continue;
        }
        if (text.compare(i, nbsp.size(), nbsp) == 0) {
            size_t run = 0;
            while (text.compare(i + run * nbsp.size(), nbsp.size(), nbsp) == 0) ++run;
            // Вне кода: ведущие держат отступ, серия из двух и более держит
            // выравнивание, одиночный в середине не значит ничего.
            const bool keep = !inCode && (leading || run > 1);
            for (size_t k = 0; k < run; ++k) out += keep ? nbsp : std::string(" ");
            i += run * nbsp.size();
            leading = false;
            continue;
        }
        if (text[i] != ' ' && text[i] != '\t') leading = false;
        out.push_back(text[i]);
        ++i;
    }
    return out;
}

namespace {

// ПИСАТЕЛЬ. Класс, а не россыпь функций с общим `out` в параметрах: у записи
// есть состояние — колонки вложенности, счётчики нумерации, живые прогоны
// списков, предыдущий блок, — и держать его в классе честнее, чем таскать
// восемью доводами.
//
// Наружу из файла не торчит ничего: единственный вход — метод
// ZDocument::toMarkdown ниже. Свободной функции «сериализовать документ» больше
// нет вовсе, и это нарочно: пока она была, все ходили мимо класса.
class Writer {
public:
    Writer(const NoteHeader& header, bool hasLinkDefs, bool wantMap)
        : hasLinkDefs_(hasLinkDefs), wantMap_(wantMap) {
        out_ += header.toBytes();
    }

    // Очередной ЛОГИЧЕСКИЙ блок заметки.
    void push(const Piece& b);

    std::string finish(std::vector<BlockLines>* map);

protected:

    std::string out_;
    // Где начался каждый блок — пока в БАЙТАХ; в номера строк переведём одним
    // проходом в конце. Так карта не мешает потоку вывода: у него полдюжины
    // мест с `continue`, и считать строки по дороге значило бы не забыть ни
    // одного из них.
    std::vector<size_t> startsAt_;
    // Колонка, с которой начинается содержимое на каждом уровне вложенности,
    // и счётчики нумерации.
    std::vector<size_t> contentCol_{0};
    std::vector<int> ordinal_{0};
    std::vector<char> runAlive_{0};      // на этом уровне прогон ещё идёт
    std::vector<char> runOrdered_{0};    // и он нумерованный
    bool prevWasQuote_ = false;
    bool hasFirst_ = false;              // хоть один блок уже записан
    Piece previous_;
    bool hasLinkDefs_ = false;
    bool wantMap_ = false;
    [[maybe_unused]] int prevLevel_ = -1;
};

void Writer::push(const Piece& b) {
    // Имена по-старому: тело переехало в метод целиком, и переименовывать в нём
    // каждую переменную значило бы сделать правку, в которой ошибку не увидеть.
    std::string& out = out_;
    std::vector<size_t>& startsAt = startsAt_;
    std::vector<size_t>& contentCol = contentCol_;
    std::vector<int>& ordinal = ordinal_;
    std::vector<char>& runAlive = runAlive_;
    std::vector<char>& runOrdered = runOrdered_;
    bool& prevWasQuote = prevWasQuote_;
    const bool hasLinkDefs = hasLinkDefs_;
    [[maybe_unused]] int& prevLevel = prevLevel_;
    const size_t i = hasFirst_ ? 1 : 0;   // «не первый» — вот и всё, что нужно
    {
        if (wantMap_) startsAt.push_back(out.size());
        validate(b);
        const std::string_view body = b.text;

        bool thisIsQuote = !b.raw && b.kind == Kind::Quote;

        // Пустая строка выводится только блоком VSpace — от себя не добавляем
        // ничего. Иначе обязательная пустая строка при следующем чтении стала бы
        // блоком VSpace, которого никто не набирал, и круг бы разошёлся. Что два
        // соседних блока не слипнутся, держит инвариант IR: между такими всегда
        // стоит VSpace (см. Document::wouldMerge).
        // Текст в пустой строке инвариант запрещает, но если он там всё же
        // оказался — печатаем его абзацем. Текст свят; потерять его нельзя ни
        // при каких обстоятельствах.
        if (!b.raw && b.kind == Kind::VSpace && body.empty()) {
            out += "\n";
            // Прогон списка пустая строка не обрывает: "- раз\n\n- два" — один
            // список, просто просторный. А вот цитату обрывает: две цитаты
            // через пустую строку — именно две, и разделять их строкой ">"
            // нельзя, она бы их снова склеила.
            prevWasQuote = false;
            previous_ = b;
            hasFirst_ = true;
            return;
        }

        // Абзацы одной цитаты разделяются строкой ">": иначе цитата развалилась
        // бы на две.
        if (i > 0 && prevWasQuote && thisIsQuote) out += ">\n";
        // Последний рубеж инварианта: если между блоками нет VSpace, а без
        // пустой строки они слипнутся, — ставим её. Такое IR неправильно, но
        // испортить файл оно не должно.
        else if (i > 0 && wouldMerge(previous_, b))
            out += "\n";

        if (b.raw) {
            out += body;
            if (out.empty() || out.back() != '\n') out.push_back('\n');
            std::fill(runAlive.begin(), runAlive.end(), 0);
            prevWasQuote = false;
            prevLevel = -1;
            previous_ = b;
            hasFirst_ = true;
            return;
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
                // Блок кода внутри пункта отступает до колонки его содержимого —
                // и забор, и каждая строка. Пустые строки внутри кода при этом
                // остаются пустыми: отступ в них дал бы концевые пробелы, а
                // блоку кода они не нужны.
                const std::string pad(indentInsideItem(b, contentCol), ' ');
                const std::string fence = fenceFor(body, b.info);
                out += pad;
                out += fence;
                out += b.info;
                out.push_back('\n');
                for (size_t at = 0; at < body.size();) {
                    size_t end = body.find('\n', at);
                    if (end == std::string_view::npos) end = body.size();
                    if (end > at) {
                        out += pad;
                        out.append(body, at, end - at);
                    }
                    out.push_back('\n');
                    at = end + 1;
                }
                out += pad;
                out += fence;
                out.push_back('\n');
                break;
            }

            case Kind::Math: {
                // ВЫКЛЮЧНАЯ ФОРМУЛА — ДОСЛОВНО. Текст блока и есть её исходник
                // вместе с долларами: ни разметки, ни экранирования внутри нет,
                // писать нечего, кроме самих байтов. Внутри пункта списка
                // отступ до колонки содержимого — как у блока кода.
                // ОТСТУП ТОЛЬКО ПЕРВОЙ СТРОКЕ. Текст блока — дословный
                // исходник, и у строк продолжения СВОИ ведущие пробелы уже
                // внутри него: маркер пункта съел отступ только у первой.
                // Приписав отступ каждой, я удваивал его на каждой записи —
                // `  a &= b` становилось `    a &= b` (поймал набор корпуса).
                const std::string pad(indentInsideItem(b, contentCol), ' ');
                bool firstLine = true;
                for (size_t at = 0; at < body.size();) {
                    size_t end = body.find('\n', at);
                    if (end == std::string_view::npos) end = body.size();
                    if (end > at) {
                        if (firstLine) out += pad;
                        out.append(body, at, end - at);
                    }
                    firstLine = false;
                    out.push_back('\n');
                    at = end + 1;
                }
                break;
            }

            case Kind::VSpace:
            case Kind::Paragraph: {
                // Блок внутри пункта: отступ до колонки его содержимого. Ровно
                // этим markdown и отличает второй абзац пункта от нового блока
                // за списком — маркера у него нет, есть только отступ.
                const size_t indent = indentInsideItem(b, contentCol);
                out.append(indent, ' ');
                TextSink sink;
                sink.contIndent = std::string(indent, ' ');
                sink.hasLinkDefs = hasLinkDefs;
                appendInlineText(sink, b);
                out += sink.out;
                out.push_back('\n');
                break;
            }

            case Kind::Quote: {
                const size_t indent = indentInsideItem(b, contentCol);
                out.append(indent, ' ');
                TextSink sink;
                sink.contIndent = std::string(indent, ' ') + "> ";
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

            case Kind::Html:
                // Пока единственный вид — комментарий: скобки — структура,
                // текст — внутренность. Крайние пробелы канонические, перенос
                // строки внутри — многострочный комментарий, он законен.
                switch (b.html) {
                    case HtmlKind::Comment: {
                        const size_t indent = indentInsideItem(b, contentCol);
                        out.append(indent, ' ');
                        if (body.empty()) {
                            out += "<!-- -->\n";
                            break;
                        }
                        out += "<!-- ";
                        // Строки внутренности с отступом блока — как строки кода.
                        for (size_t at = 0; at < body.size();) {
                            size_t end = body.find('\n', at);
                            if (end == std::string_view::npos) end = body.size();
                            if (at > 0) out.append(indent, ' ');
                            out.append(body, at, end - at);
                            if (end < body.size()) out.push_back('\n');
                            at = end + 1;
                        }
                        out += " -->\n";
                        break;
                    }
                }
                break;

            case Kind::Divider:
                // Текст свят: разделителю он не положен, но если он там всё же
                // оказался — печатаем абзацем, как это делает пустая строка.
                if (!body.empty()) {
                    TextSink sink;
                    sink.hasLinkDefs = hasLinkDefs;
                    appendInlineText(sink, b);
                    out += sink.out;
                    out.push_back('\n');
                    break;
                }
                out += "___\n";
                break;

            case Kind::ListItem: {
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
                bool ord = b.marker == Marker::Ordered;
                bool sameRun = runAlive[level] && (runOrdered[level] != 0) == ord;
                ordinal[level] = sameRun ? ordinal[level] + 1 : 1;
                runAlive[level] = 1;
                runOrdered[level] = ord ? 1 : 0;
                for (size_t k = level + 1; k < ordinal.size(); ++k) runAlive[k] = 0;

                std::string marker = markerFor(b, ordinal[level]);
                size_t indent = contentCol[level];
                size_t childIndent = indent + markerIndentWidth(b, ordinal[level]);
                contentCol[level + 1] = childIndent;

                out.append(indent, ' ');
                if (body.empty()) {
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

        // Прогон списка обрывает только блок, вышедший из списка. Второй абзац
        // пункта список не заканчивает: нумерация за ним продолжается.
        const bool insideList = !b.raw && b.level >= 0;
        if (!insideList) std::fill(runAlive.begin(), runAlive.end(), 0);
        prevWasQuote = thisIsQuote;
        prevLevel = insideList ? b.level : -1;
    }
    previous_ = b;
    hasFirst_ = true;
}

std::string Writer::finish(std::vector<BlockLines>* map) {
    std::string& out = out_;
    const std::vector<size_t>& startsAt = startsAt_;
    if (map != nullptr) {
        map->assign(startsAt.size(), BlockLines{});
        // Смещения не убывают, поэтому строки считаются одним проходом по
        // выводу: идём по нему, отмечая границы блоков там, где они попались.
        size_t at = 0;
        int line = 0;
        std::vector<int> lineAt(startsAt.size() + 1, 0);
        for (size_t k = 0; k < startsAt.size(); ++k) {
            while (at < startsAt[k]) {
                if (out[at] == '\n') ++line;
                ++at;
            }
            lineAt[k] = line;
        }
        while (at < out.size()) {
            if (out[at] == '\n') ++line;
            ++at;
        }
        lineAt[startsAt.size()] = line;
        for (size_t k = 0; k < startsAt.size(); ++k) {
            (*map)[k].first = lineAt[k];
            (*map)[k].count = lineAt[k + 1] - lineAt[k];
        }
    }
    return out;
}




}  // namespace

// --- ОБХОД ЖИВОГО ДОКУМЕНТА ------------------------------------------------
//
// Логический блок заметки — не то же, что QTextBlock: литеральные куски (код,
// дословное) лежат в документе ПОСТРОЧНО, по блоку на строку, и склеиваются
// здесь обратно. Признак продолжения обязателен: без него разрезанный блок кода
// из двух строк неотличим от двух блоков кода подряд, а это разный markdown.

namespace {

std::string toUtf8(const QString& text) {
    const QByteArray utf8 = text.toUtf8();
    return std::string(utf8.constData(), size_t(utf8.size()));
}

// Является ли этот текст ОДНОЙ формулой целиком. Спрашивается общий канон, а не
// «начинается с доллара»: иначе вид и разбор разошлись бы на первом же краю.
bool wholeMath(const std::string& text) {
    const std::vector<MathSpan> found = scanMath(text);
    return found.size() == 1 && found.front().start == 0 &&
           size_t(found.front().end) == text.size();
}

bool sameStyle(const Run& a, const Run& b) {
    return a.flags == b.flags && a.href == b.href && a.title == b.title;
}

// Пустой документ Qt не бывает: один блок в нём есть всегда. Свойств у этого
// блока нет — их ставит сборщик, а ему нечего было ставить.
bool isPhantomBlock(const QTextDocument& doc, const QTextBlock& block) {
    if (doc.blockCount() != 1 || !block.text().isEmpty()) return false;
    const QTextBlockFormat format = block.blockFormat();
    return !format.hasProperty(KindProperty) && !format.hasProperty(RawProperty);
}

// Один QTextBlock: и текст, и куски с начертанием. Смещение копится в байтах —
// куски идут подряд и покрывают блок целиком.
//
// Разделитель строк превращается обратно в исходный знак ТОЛЬКО там, где стоит
// пометка BreakSourceProperty. Без неё это чужой U+2028 из самого текста
// заметки — заметки из Apple Notes им кишат, — и трогать его нельзя.
void gatherLine(const QTextBlock& block, Piece& piece, bool withRuns) {
    for (QTextBlock::iterator it = block.begin(); !it.atEnd(); ++it) {
        const QTextFragment fragment = it.fragment();
        if (!fragment.isValid()) continue;

        const QTextCharFormat format = fragment.charFormat();
        QString text = fragment.text();
        switch (format.intProperty(BreakSourceProperty)) {
            case BreakNewline:
                text.replace(QChar::LineSeparator, QLatin1Char('\n'));
                break;
            case BreakCarriageReturn:
                text.replace(QChar::LineSeparator, QLatin1Char('\r'));
                break;
            case BreakParagraph:
                text.replace(QChar::LineSeparator, QChar(QChar::ParagraphSeparator));
                break;
            default:
                break;   // чужой U+2028 из самого текста — трогать нельзя
        }

        // ОБЪЕКТ ОТДАЁТ СВОЙ ИСХОДНИК, А НЕ СЕБЯ. В документе он занимает один
        // знак U+FFFC; наружу — в файл, в буфер обмена, в журнал — уходит то,
        // что написано в файле, и оно лежит тут же, в свойствах формата.
        // Правило железное: U+FFFC не покидает QTextDocument (инцидент №15).
        if (format.objectType() == ImageObject) {
            const int32_t at = int32_t(piece.text.size());
            const QString alt = format.property(ObjectAltProperty).toString();
            const bool wiki = !format.hasProperty(ObjectAltProperty);
            piece.text += toUtf8(wiki ? format.property(ObjectSourceProperty).toString() : alt);
            if (!withRuns || wiki) continue;
            Run run;
            run.start = at;
            run.end = int32_t(piece.text.size());
            run.flags = InlineImage;
            run.href = toUtf8(format.anchorHref());
            run.title = toUtf8(format.property(SpanTitleProperty).toString());
            piece.runs.push_back(std::move(run));
            continue;
        }

        const std::string bytes = toUtf8(text);
        if (bytes.empty()) continue;

        const int32_t offset = int32_t(piece.text.size());
        piece.text += bytes;
        if (!withRuns) continue;

        const int style = format.intProperty(SpanStyleProperty);
        const QString href = format.anchorHref();
        if (style == 0 && href.isEmpty()) continue;

        Run run;
        run.start = offset;
        run.end = offset + int32_t(bytes.size());
        run.set(InlineBold, (style & SpanBold) != 0);
        run.set(InlineItalic, (style & SpanItalic) != 0);
        run.set(InlineStrike, (style & SpanStrike) != 0);
        run.set(InlineCode, (style & SpanCode) != 0);
        run.href = toUtf8(href);

        // Подпись картинки плоская по построению: правки могли домешать в
        // формат другие биты — здесь они гасятся, иначе вышло бы то, что файл
        // выразить не может. Картинка без пути — не картинка.
        run.set(InlineImage, (style & SpanImage) != 0 && !run.href.empty());
        if (run.image()) {
            run.flags = InlineImage;
            run.title = toUtf8(format.property(SpanTitleProperty).toString());
        }

        // Строчный комментарий плоский так же; внутренность с "-->" файл
        // выразить не может — такой кусок перестаёт быть комментарием и
        // становится видимым текстом (писатель его экранирует).
        run.set(InlineComment, (style & SpanComment) != 0 && !run.image() &&
                                   bytes.find("-->") == std::string::npos);
        if (run.comment()) {
            run.flags = InlineComment;
            run.href.clear();
            run.title.clear();
        }

        // ФОРМУЛА ОБЯЗАНА ПЕРЕЖИТЬ КРУГ. Не переживёт — файл испортится молча
        // при первой же записи: `$\gamma$` перестаёт быть математикой, и
        // писатель экранирует косую. Каждая запись удваивает, и заметка
        // обрастает косыми (нашёл владелец, архивируя заметку с формулами).
        //
        // Здесь спрашивается ТОЛЬКО бит стиля. Целостность проверяется ПОСЛЕ
        // склейки кусков: куски рвутся где угодно (мягкий перенос, другой кегль
        // у эмодзи), и `$\begin{aligned}` в одном куске формулой не выглядит
        // никогда.
        run.set(InlineMath, (style & SpanMath) != 0 && !run.image() && !run.comment());
        if (run.math()) {
            run.flags = InlineMath;
            run.href.clear();
            run.title.clear();
        }

        // Куски дробятся и без смены начертания: мягкий перенос помечен
        // отдельно, эмодзи набраны другим кеглем. Такие соседи склеиваются.
        if (!piece.runs.empty() && sameStyle(piece.runs.back(), run) &&
            piece.runs.back().end == run.start) {
            piece.runs.back().end = run.end;
        } else {
            piece.runs.push_back(std::move(run));
        }
    }
}

// Склейка позади — теперь канон. Кусок, переставший быть формулой (правка
// разорвала её пополам, доллар потерялся), становится обычным текстом.
void settleMath(Piece& piece) {
    for (Run& run : piece.runs) {
        if (!run.math()) continue;
        if (!wholeMath(std::string(piece.view(run)))) run.flags = 0;
    }
}

// Комментарий держит свой инвариант на границе документ→файл: разметки внутри
// не бывает, а внутренность с "-->" файл выразить не может — такой блок
// перестаёт быть комментарием и становится видимым текстом.
void settleComment(Piece& piece) {
    if (piece.raw || piece.kind != Kind::Html) return;
    if (piece.text.find("-->") != std::string::npos)
        piece.kind = Kind::Paragraph;
    else
        piece.runs.clear();
}

}  // namespace

// ОБЩАЯ СТУПЕНЬ «ЖИВОЙ ДОКУМЕНТ → ЛОГИЧЕСКИЕ БЛОКИ». Обратная к parsePieces, и
// такая же одна на всех: склейка литеральных строк, дословные куски, доводка
// формул и комментариев — правила границы «документ → файл», и второй их копии
// быть не должно.
void walkPieces(const QTextDocument& doc, const std::function<bool(const Piece&)>& sink,
                int fromBlock, int toBlock) {
    Piece piece;
    bool open = false;
    bool stop = false;

    auto close = [&] {
        if (!open) return;
        // У дословного куска рода нет: он остаётся абзацем, а текст блока и есть
        // его дословные байты.
        if (piece.raw) {
            piece.kind = Kind::Paragraph;
            piece.info.clear();
        }
        settleComment(piece);
        settleMath(piece);
        if (!sink(piece)) stop = true;
        piece = Piece{};
        open = false;
    };

    QTextBlock start = fromBlock > 0 ? doc.findBlockByNumber(fromBlock) : doc.begin();
    for (QTextBlock block = start;
         block.isValid() && !stop && (toBlock < 0 || block.blockNumber() <= toBlock);
         block = block.next()) {
        if (isPhantomBlock(doc, block)) continue;

        const QTextBlockFormat format = block.blockFormat();
        const bool raw = isRawBlock(block);

        if (isContinuationBlock(block) && open) {
            piece.text.push_back('\n');
            gatherLine(block, piece, false);
        } else {
            close();
            open = true;
            piece.raw = raw;
            if (!raw) {
                piece.kind = kindOf(block);
                if (piece.kind == Kind::Heading) piece.headingLevel = format.headingLevel();
                if (isList(piece.kind)) {
                    const MarkerStyle style = markerOf(block);
                    piece.marker = style.marker;
                    piece.checked = style.checked;
                }
                piece.level = levelOf(block);
                if (piece.kind == Kind::Code)
                    piece.info = toUtf8(format.stringProperty(InfoProperty));
            }
            // Разметку внутри блока кода не читаем: содержимое там буквальное.
            gatherLine(block, piece, !raw && piece.kind != Kind::Code);
        }

        // Признак стоит на последней строке блока — там, где перевод и был.
        //
        // Кладём его И В ТЕКСТ, И В ПРИЗНАК. В текст — писателю: он печатает
        // байты и про признак не знает. В признак — сборщику: обход обязан
        // быть точной обратной стороной разбора, иначе блоки, снятые с живого
        // документа и положенные обратно, теряли бы этот перевод строки, и
        // пустая строка в конце блока кода исчезала бы при каждой операции.
        if (format.boolProperty(TrailingNewlineProperty)) {
            piece.text.push_back('\n');
            piece.trailingNewline = true;
        }
    }
    close();
}

namespace {

// Единственное место, где живая заметка превращается в байты. Ходит прямо по
// внутреннему QTextDocument — ни промежуточного представления, ни второй живой
// модели.
std::string writeInto(const QTextDocument& doc, const NoteHeader& header,
                      std::vector<BlockLines>* map) {

    // Есть ли в заметке ссылочные определения — от этого зависит экранирование
    // квадратных скобок. Спрашивается ДО записи, потому что ответ нужен уже на
    // первом блоке. Проверка идёт построчно, поэтому смотреть на несклеенные
    // строки можно: ответ тот же.
    bool hasLinkDefs = false;
    for (QTextBlock b = doc.begin(); b.isValid() && !hasLinkDefs; b = b.next())
        if (isRawBlock(b) && looksLikeLinkDefinition(toUtf8(b.text()))) hasLinkDefs = true;

    Writer writer(header, hasLinkDefs, map != nullptr);
    walkPieces(doc, [&](const Piece& piece) {
        writer.push(piece);
        return true;
    });
    return writer.finish(map);
}

}  // namespace

// Блоки, заметкой ещё не ставшие, — кусок в буфере обмена. Кладём их в
// документ-однодневку и записываем тем же писателем: правил записи двух не
// бывает, а собрать и обойти кусок выделения стоит микросекунды.
std::string writePieces(const std::vector<Piece>& blocks, const NoteHeader& header,
                        std::vector<BlockLines>* map) {
    QTextDocument temp;
    buildDocument(blocks, temp);
    return writeInto(temp, header, map);
}

std::string ZDocument::toMarkdown() const {
    return writeInto(d_->text, d_->header, nullptr);
}

std::string ZDocument::bodyMarkdown() const {
    return writeInto(d_->text, NoteHeader{}, nullptr);
}

std::vector<SourceLine> ZDocument::sourceLines() const {
    // Тело БЕЗ шапки: в ней живёт `modified`, она меняется при каждой записи, и
    // всякая разность начиналась бы с неё — всегда одной и той же строки.
    std::vector<BlockLines> map;
    const std::string text = writeInto(d_->text, NoteHeader{}, &map);

    const QString whole = QString::fromUtf8(text.data(), qsizetype(text.size()));
    const QStringList lines = whole.split(QLatin1Char('\n'));

    std::vector<SourceLine> out;
    out.reserve(size_t(lines.size()));
    for (const QString& line : lines) out.push_back(SourceLine{line, -1});
    for (size_t block = 0; block < map.size(); ++block) {
        for (int k = 0; k < map[block].count; ++k) {
            const int line = map[block].first + k;
            if (line >= 0 && size_t(line) < out.size()) out[size_t(line)].block = int(block);
        }
    }
    return out;
}

}  // namespace zametti
