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
#include "list_line.h"
#include "math_scan.h"

#include <QChar>
#include <QLatin1String>
#include <QString>
#include <QStringView>
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

// Слипнутся ли два блока, окажись они в файле подряд без пустой строки. Правило
// ОДНО НА ВСЕХ и живёт в block_kind.h — здесь только перевод вопроса с языка
// логических блоков. Своя копия правила тут и лежала, слово в слово; копия
// правила — это правило, которое однажды разойдётся с оригиналом.
bool wouldMerge(const Piece& previous, const Piece& next) {
    // Пуст ли следующий пункт: текст пуст, картинки в нём нет (у картинки без
    // подписи содержимое — сам снимок, а не текст), и это не задача — у неё
    // содержимое сам чекбокс.
    bool empty = next.text.isEmpty() && next.marker != Marker::Task;
    for (const Run& run : next.runs)
        if (run.image()) empty = false;
    return zametti::wouldMerge(previous.kind, previous.raw, previous.isClosedHtmlComment(),
                               next.kind, next.raw, next.level, empty);
}

}  // namespace


namespace {

// Писатель работает НАД ТЕКСТОМ (QString), а не над байтами: всё, что в памяти,
// — UTF-16 (решение владельца, сессия refactor2), и байты появляются только на
// границе файла. Правила ниже — ASCII-правила CommonMark, и им всё равно, в
// каких единицах лежит текст: не-ASCII знак — обычная буква.
bool isAsciiSpace(QChar c) {
    return c == u' ' || c == u'\t' || c == u'\n' || c == u'\r' || c == u'\v' || c == u'\f';
}

bool isAsciiPunct(QChar c) {
    const char16_t u = c.unicode();
    return (u >= u'!' && u <= u'/') || (u >= u':' && u <= u'@') || (u >= u'[' && u <= u'`') ||
           (u >= u'{' && u <= u'~');
}

bool isDigit(QChar c) { return c >= u'0' && c <= u'9'; }
bool isAsciiAlpha(QChar c) { return (c >= u'a' && c <= u'z') || (c >= u'A' && c <= u'Z'); }
bool isAsciiAlnum(QChar c) { return isAsciiAlpha(c) || isDigit(c); }

// Есть ли этот знак среди перечисленных (ASCII).
bool oneOf(QChar c, const char* set) {
    if (c.unicode() >= 0x80) return false;
    for (; *set != '\0'; ++set)
        if (c == QLatin1Char(*set)) return true;
    return false;
}

// Классы символов для правил «фланкирования» из CommonMark. Не-ASCII считаем
// обычной буквой: для кириллицы это верно, а редкие случаи юникодной пунктуации
// приведут лишь к одному лишнему обратному слэшу, а не к потере смысла.
bool flankWhitespace(QStringView s, qsizetype i, bool atEdge) {
    if (atEdge) return true;
    return isAsciiSpace(s.at(i));
}

bool flankPunct(QStringView s, qsizetype i, bool atEdge) {
    if (atEdge) return false;
    return isAsciiPunct(s.at(i));
}

// Слово с точки зрения markdown: то, что не пробел и не ASCII-пунктуация.
// Не-ASCII знаки сюда попадают целиком — для кириллицы это верно.
bool isWordChar(QChar c) { return !isAsciiSpace(c) && !isAsciiPunct(c); }

// Может ли прогон из delim открыть или закрыть выделение в этом месте.
bool runCanDelimit(QStringView s, qsizetype begin, qsizetype end) {
    bool prevEdge = (begin == 0);
    bool nextEdge = (end >= s.size());
    qsizetype prev = prevEdge ? 0 : begin - 1;
    qsizetype next = nextEdge ? 0 : end;

    // Начало и конец строки внутри текста тоже считаем «пробелом».
    if (!prevEdge && s.at(prev) == u'\n') prevEdge = true;
    if (!nextEdge && s.at(next) == u'\n') nextEdge = true;

    bool prevWs = flankWhitespace(s, prev, prevEdge);
    bool nextWs = flankWhitespace(s, next, nextEdge);
    bool prevPu = flankPunct(s, prev, prevEdge);
    bool nextPu = flankPunct(s, next, nextEdge);

    bool leftFlanking = !nextWs && (!nextPu || prevWs || prevPu);
    bool rightFlanking = !prevWs && (!prevPu || nextWs || nextPu);

    // У '_' правила строже: внутри слова он выделения не образует. Без этого
    // уточнения каждый snake_case в заметках обрастал бы слэшами.
    if (s.at(begin) == u'_') {
        bool canOpen = leftFlanking && (!rightFlanking || prevPu);
        bool canClose = rightFlanking && (!leftFlanking || nextPu);
        return canOpen || canClose;
    }
    return leftFlanking || rightFlanking;
}

bool looksLikeEntity(QStringView s, qsizetype i) {
    qsizetype j = i + 1;
    if (j < s.size() && s.at(j) == u'#') {
        ++j;
        if (j < s.size() && (s.at(j) == u'x' || s.at(j) == u'X')) ++j;
    }
    qsizetype digitsBegin = j;
    while (j < s.size() && isAsciiAlnum(s.at(j))) ++j;
    return j > digitsBegin && j < s.size() && s.at(j) == u';';
}

bool startsHtmlish(QStringView s, qsizetype i) {
    qsizetype j = i + 1;
    if (j >= s.size()) return false;
    const QChar c = s.at(j);
    return isAsciiAlpha(c) || c == u'/' || c == u'!' || c == u'?';
}

// Скобку надо экранировать только если она действительно открывает ссылку:
// "[x]" сам по себе — обычный текст, ссылкой он станет лишь при определении
// вида "[x]: /a", а таких мы не выводим. Экранировать всё подряд нельзя:
// "1. [x] текст" обязано вернуться байт в байт.
bool bracketOpensLink(QStringView s, qsizetype i) {
    int depth = 0;
    for (qsizetype j = i; j < s.size(); ++j) {
        if (s.at(j) == u'\\') { ++j; continue; }
        if (s.at(j) == u'\n') return false;
        if (s.at(j) == u'[') ++depth;
        else if (s.at(j) == u']') {
            if (--depth == 0) return j + 1 < s.size() && s.at(j + 1) == u'(';
        }
    }
    return false;
}

// Просто парная скобка, без разбора того, во что она превратится.
bool bracketPairAt(QStringView s, qsizetype i) {
    int depth = 0;
    for (qsizetype j = i; j < s.size(); ++j) {
        if (s.at(j) == u'\\') { ++j; continue; }
        if (s.at(j) == u'\n') return false;
        if (s.at(j) == u'[') ++depth;
        else if (s.at(j) == u']' && --depth == 0) return true;
    }
    return false;
}

qsizetype lineEndFrom(QStringView s, qsizetype i) {
    const qsizetype e = s.indexOf(u'\n', i);
    return e < 0 ? s.size() : e;
}

// Начало строки: конструкции, которые захватывают всю строку и потому меняют
// разбор, если их не экранировать.
bool needsLineStartEscape(QStringView s, qsizetype i, qsizetype& escapeAt) {
    const qsizetype e = lineEndFrom(s, i);
    const QChar c = s.at(i);

    if (c == u'>') { escapeAt = i; return true; }

    if (c == u'#') {
        qsizetype j = i;
        while (j < e && s.at(j) == u'#') ++j;
        if (j - i <= 6 && (j == e || s.at(j) == u' ' || s.at(j) == u'\t')) { escapeAt = i; return true; }
        return false;
    }

    if (c == u'-' || c == u'+' || c == u'*') {
        if (i + 1 == e || s.at(i + 1) == u' ' || s.at(i + 1) == u'\t') { escapeAt = i; return true; }
    }

    if (isDigit(c)) {
        qsizetype j = i;
        while (j < e && isDigit(s.at(j))) ++j;
        if (j - i <= 9 && j < e && (s.at(j) == u'.' || s.at(j) == u')') &&
            (j + 1 == e || s.at(j + 1) == u' ' || s.at(j + 1) == u'\t')) {
            escapeAt = j;
            return true;
        }
    }

    // "[метка]: /url" в начале строки — определение ссылки, то есть целый блок,
    // а не текст. Внутри строки та же последовательность безобидна.
    if (c == u'[') {
        qsizetype j = i + 1;
        while (j < e && s.at(j) != u']') {
            if (s.at(j) == u'\\') ++j;
            ++j;
        }
        if (j + 1 < e && s.at(j) == u']' && s.at(j + 1) == u':') { escapeAt = i; return true; }
    }

    // Три и больше тильд в начале строки — открывающий забор блока кода.
    // Обычные правила фланкирования этого не ловят: у строки из одних тильд
    // с обеих сторон «пробел», и экранировать её вроде бы не за что.
    if (c == u'~' || c == u'`') {
        qsizetype j = i;
        while (j < e && s.at(j) == c) ++j;
        if (j - i >= 3) { escapeAt = i; return true; }
    }

    // Строка целиком из '=' или из '-'/'_'/'*' — setext-подчёркивание или
    // тематический разделитель.
    if (c == u'=' || c == u'-' || c == u'_' || c == u'*') {
        bool uniform = true;
        qsizetype cnt = 0;
        for (qsizetype j = i; j < e; ++j) {
            if (s.at(j) == c) { ++cnt; continue; }
            if (s.at(j) == u' ' || s.at(j) == u'\t' || s.at(j) == u'\r') continue;
            uniform = false;
            break;
        }
        if (uniform && cnt > 0) { escapeAt = i; return true; }
    }
    return false;
}

// Пометки по знакам текста, которые нельзя вывести из самого текста: они
// зависят от того, какие ограничители сериализатор поставит вокруг.
enum Mark : unsigned char {
    kMarkDelimEdge = 1,   // край выделения: '*'/'_'/'~' здесь слипнется с ограничителем
    kMarkInLink    = 2,   // текст ссылки: скобки внутри разорвут её
};

struct TextSink {
    QString out;
    QString contIndent;   // отступ строк-продолжений
    bool bol = true;      // стоим в начале строки (после маркера/отступа)
    const std::vector<unsigned char>* marks = nullptr;
    bool hasLinkDefs = false; // в документе есть "[x]: /url" — значит любая пара
                              // скобок может внезапно стать ссылкой
    // Текст пойдёт сразу за маркером БУЛЛЕТА (у номера чекбокс задачей не
    // становится — проверено кругом: "1. [x] текст" так и остаётся текстом).
    // Чекбокс, оказавшийся первым в тексте, сделал бы из буллета задачу —
    // человек набрал "[ ] " в пункте, а из файла вернулась бы задача, и текст
    // потерял бы эти четыре знака. Снимается после первой же строки:
    // продолжение пункта чекбоксом не становится.
    bool taskBoxAhead = false;
};

// Чекбокс задачи в начале текста: "[ ] ", "[x] ", "[X] " или он же в конец
// строки. Ровно то, что GFM читает задачей сразу за маркером пункта.
bool looksLikeTaskBox(QStringView text, qsizetype at) {
    if (at + 2 >= text.size() || text.at(at) != u'[') return false;
    const QChar inside = text.at(at + 1);
    if (inside != u' ' && inside != u'x' && inside != u'X') return false;
    if (text.at(at + 2) != u']') return false;
    const qsizetype after = at + 3;
    return after >= text.size() || text.at(after) == u' ' || text.at(after) == u'\t' ||
           text.at(after) == u'\n';
}

unsigned char markAt(const TextSink& sink, qsizetype i) {
    return (sink.marks != nullptr && size_t(i) < sink.marks->size()) ? (*sink.marks)[size_t(i)] : 0;
}

// Кладёт [begin, end) текста в вывод, экранируя ровно то, что иначе изменит
// разбор. Контекст (соседние символы) берётся из полного текста, чтобы разрез
// на прогоны стилей не влиял на решения.
// Начал бы этот доллар формулу, если оставить его голым? Спрашиваем ОБЩИЙ
// канон (math_scan.h), а не гадаем: у сериализатора и у разбора правило одно,
// иначе они разойдутся молча.
bool dollarOpensMath(QStringView text, qsizetype at) {
    for (const MathSpan& span : scanMath(text.mid(at)))
        return span.start == 0;   // первая найденная либо здесь, либо дальше
    return false;
}

void appendEscaped(TextSink& sink, QStringView text, qsizetype begin, qsizetype end) {
    qsizetype i = begin;
    while (i < end) {
        const QChar c = text.at(i);

        if (c == u'\n') {
            sink.out += u'\n';
            sink.out += sink.contIndent;
            sink.bol = true;
            ++i;
            continue;
        }

        if (sink.bol) {
            if (sink.taskBoxAhead) {
                sink.taskBoxAhead = false;
                // Пробелы (и неразрывные) между маркером и чекбоксом чекбокса
                // не отменяют: md4c читает "-  [ ]" задачей ровно так же, как
                // "- [ ]", а неразрывные при этом ещё и исчезают. Поэтому
                // смотрим на первый ЗНАЧАЩИЙ знак, а экранируем — его.
                qsizetype box = i;
                while (box < end && (text.at(box) == u' ' || text.at(box) == u'\t' ||
                                     text.at(box) == QChar::Nbsp))
                    ++box;
                if (box < end && looksLikeTaskBox(text, box)) {
                    sink.out += text.mid(i, box - i);
                    sink.out += u'\\';
                    sink.out += text.at(box);
                    i = box + 1;
                    sink.bol = false;
                    continue;
                }
            }
            qsizetype at = 0;
            if (needsLineStartEscape(text, i, at) && at < end) {
                sink.out += text.mid(i, at - i);
                sink.out += u'\\';
                sink.out += text.at(at);
                i = at + 1;
                sink.bol = false;
                continue;
            }
            sink.bol = false;
        }

        switch (c.unicode()) {
            case u'\\':
                sink.out += QLatin1String("\\\\");
                ++i;
                continue;
            case u'`':
                sink.out += QLatin1String("\\`");
                ++i;
                continue;
            case u'[':
                if (bracketOpensLink(text, i) || (markAt(sink, i) & kMarkInLink) != 0 ||
                    (sink.hasLinkDefs && bracketPairAt(text, i)))
                    sink.out += u'\\';
                sink.out += u'[';
                ++i;
                continue;
            case u']':
                if ((markAt(sink, i) & kMarkInLink) != 0) sink.out += u'\\';
                sink.out += u']';
                ++i;
                continue;
            case u'$':
                // ДОЛЛАР ЭКРАНИРУЕТСЯ, ТОЛЬКО ЕСЛИ ОН НАЧАЛ БЫ ФОРМУЛУ. Иначе
                // цены («заплатил $5») обросли бы косыми на ровном месте, а
                // это тот самый шум в файле, которого формат избегает. Но
                // молча отдать `\$x\$` обратно как `$x$` нельзя: при чтении
                // это станет математикой, и текст поменяет смысл.
                if (dollarOpensMath(text, i)) sink.out += u'\\';
                sink.out += u'$';
                ++i;
                continue;
            case u'<':
                if (startsHtmlish(text, i)) sink.out += u'\\';
                sink.out += u'<';
                ++i;
                continue;
            case u'&':
                if (looksLikeEntity(text, i)) sink.out += u'\\';
                sink.out += u'&';
                ++i;
                continue;
            case u'*':
            case u'_':
            case u'~': {
                qsizetype j = i;
                while (j < text.size() && text.at(j) == c) ++j;
                bool escape = runCanDelimit(text, i, j);
                // ОДИНОКАЯ ТИЛЬДА ЗАЧЁРКИВАНИЯ НЕ ОТКРОЕТ: ей нужна пара, а
                // другой тильды в тексте нет. Без этого «~подпись» — спрятанная
                // подпись картинки (см. isNonameCaption) — уезжала бы в файл
                // как «\~подпись», и написанное человеком руками менялось бы
                // при первой же записи. Судья — md4c: пробег без пары читается
                // буквально (Roundtrip/Corpus стерегут).
                if (escape && c == u'~' && text.indexOf(c, j) < 0 &&
                    (i == 0 || text.left(i).indexOf(c) < 0))
                    escape = false;
                for (qsizetype k = i; k < j && !escape; ++k)
                    if ((markAt(sink, k) & kMarkDelimEdge) != 0) escape = true;
                const qsizetype stop = j < end ? j : end;
                for (qsizetype k = i; k < stop; ++k) {
                    if (escape) sink.out += u'\\';
                    sink.out += c;
                }
                i = stop;
                continue;
            }
            default:
                sink.out += c;
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
QLatin1String italicDelim(QStringView text, qsizetype begin, qsizetype end) {
    const bool leftIntraword = begin > 0 && isWordChar(text.at(begin - 1));
    const bool rightIntraword = end < text.size() && isWordChar(text.at(end));
    return (leftIntraword || rightIntraword) ? QLatin1String("*") : QLatin1String("_");
}

bool hrefNeedsBrackets(QStringView h) {
    for (const QChar c : h)
        if (isAsciiSpace(c) || c == u'(' || c == u')' || c == u'<' || c == u'>' ||
            c.unicode() < 0x20)
            return true;
    return h.isEmpty();
}

bool looksLikeEmail(QStringView text) {
    const qsizetype at = text.indexOf(u'@');
    return at > 0 && at + 1 < text.size() && text.indexOf(u'@', at + 1) < 0 &&
           text.indexOf(u'.', at) >= 0;
}

bool hasScheme(QStringView text, bool requireSlashes) {
    const qsizetype colon = text.indexOf(u':');
    if (colon <= 0) return false;
    for (qsizetype i = 0; i < colon; ++i) {
        const QChar c = text.at(i);
        const bool ok = isAsciiAlnum(c) || c == u'+' || c == u'-' || c == u'.';
        if (!ok) return false;
    }
    return !requireSlashes || text.mid(colon, 3) == QLatin1String("://");
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
bool bareSafe(QStringView text) {
    if (text.isEmpty()) return false;
    for (const QChar c : text) {
        // Не-ASCII в голой ссылке разбор не принимает: "https://x/путь" без
        // угловых скобок остаётся обычным текстом.
        if (c.unicode() <= u' ' || c.unicode() >= 0x80) return false;
        if (oneOf(c, "`*_~\\[]<>&()\"'")) return false;
    }
    // Хвостовую пунктуацию разбор в ссылку не включает, и голый вывод потерял бы
    // её из адреса.
    return !oneOf(text.back(), ".,;:!?");
}

// Голой ссылкой разбор считает только адрес с настоящим доменом: "https://../"
// в угловых скобках ссылка, а без них — обычный текст.
bool hasRealHost(QStringView text) {
    qsizetype begin = text.indexOf(QLatin1String("://"));
    if (begin < 0) return false;
    begin += 3;
    qsizetype end = begin;
    while (end < text.size() && !oneOf(text.at(end), "/?#")) ++end;
    if (begin >= end) return false;
    if (!isAsciiAlnum(text.at(begin))) return false;
    for (qsizetype i = begin; i + 1 < end; ++i)
        if (text.at(i) == u'.' && isAsciiAlnum(text.at(i + 1))) return true;
    return false;
}

// Что стоит СРАЗУ ЗА адресом, решает не меньше самого адреса: замерено на md4c,
// что "http://a.org/x: хвост" не ссылка вовсе — двоеточие читается началом
// схемы, — и то же самое делают '#', '"', '@', '=', '&', '%', '|', '$', '^',
// '{'. А точка, запятая, точка с запятой, восклицательный и вопросительный
// знаки, скобка, косая, плюс и дефис голую ссылку не рвут.
//
// Поэтому список ЗАКРЫТЫЙ и разрешительный: не уверены — пишем в угловых
// скобках. Лишние две скобки стоят ничего, потерянная ссылка — правки человека.
bool bareTailSafe(QChar next) {
    if (next.isNull() || next.isSpace()) return true;
    return oneOf(next, ".,;!?)/+-");
}

// Голые ссылки разбор опознаёт и без разметки, поэтому и выводить их надо
// голыми: обернув "https://x" в угловые скобки, мы переписали бы каждую заметку,
// где ссылка просто набрана в строку. Но опознаёт он не всё подряд — только три
// схемы, "www." и почту, поэтому список здесь закрытый, а не "любая схема".
LinkShape linkShape(QStringView text, QStringView href, QChar next) {
    if (text.isEmpty()) return LinkShape::Inline;
    for (const QChar c : text)
        if (isAsciiSpace(c) || c == u'<' || c == u'>') return LinkShape::Inline;

    if (href == text) {
        const bool bareScheme = text.startsWith(QLatin1String("http://")) ||
                                text.startsWith(QLatin1String("https://")) ||
                                text.startsWith(QLatin1String("ftp://"));
        if (bareScheme && bareSafe(text) && hasRealHost(text) && bareTailSafe(next))
            return LinkShape::Bare;
        if (hasScheme(text, /*requireSlashes=*/false)) return LinkShape::Angle;
        return LinkShape::Inline;
    }

    if (href.size() == text.size() + 7 && href.startsWith(QLatin1String("mailto:")) &&
        href.mid(7) == text && looksLikeEmail(text) && bareSafe(text) && bareTailSafe(next)) {
        return LinkShape::Bare;
    }

    if (href.size() == text.size() + 7 && href.startsWith(QLatin1String("http://")) &&
        href.mid(7) == text && text.startsWith(QLatin1String("www.")) && bareSafe(text) &&
        bareTailSafe(next)) {
        return LinkShape::Bare;
    }
    return LinkShape::Inline;
}

void appendHref(QString& out, QStringView href) {
    if (!hrefNeedsBrackets(href)) {
        out += href;
        return;
    }
    out += u'<';
    for (const QChar c : href) {
        if (c == u'<' || c == u'>' || c == u'\\') out += u'\\';
        out += c;
    }
    out += u'>';
}

void appendCodeSpan(QString& out, QStringView content) {
    qsizetype longest = 0;
    qsizetype run = 0;
    for (const QChar c : content) {
        run = (c == u'`') ? run + 1 : 0;
        if (run > longest) longest = run;
    }
    const QString ticks(longest + 1, u'`');

    bool onlySpace = true;
    for (const QChar c : content)
        if (c != u' ' && c != u'\t' && c != u'\n') { onlySpace = false; break; }
    const bool pad = !content.isEmpty() &&
                     (content.front() == u'`' || content.back() == u'`' ||
                      (isAsciiSpace(content.front()) && isAsciiSpace(content.back()) && !onlySpace));

    out += ticks;
    if (pad) out += u' ';
    out += content;
    if (pad) out += u' ';
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
    qsizetype begin = 0;
    qsizetype end = 0;
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
        case kHref:   return !s->href.isEmpty();
        case kCode:   return s->code();
        default:      return false;
    }
}

// Смещения спанов относительные — от начала текста блока, — поэтому сегменты
// индексируют ровно этот текст, и пересчитывать ничего не нужно.
std::vector<Segment> splitIntoSegments(const Piece& b) {
    const qsizetype textSize = b.text.size();
    std::vector<Segment> segs;
    qsizetype pos = 0;
    for (const Run& s : b.runs) {
        const qsizetype so = s.start;
        const qsizetype se = s.end;
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
    const QString& href = segs[i].span->href;
    size_t j = i;
    while (j < hi && hasAttr(segs[j].span, attr) &&
           (attr != kHref || segs[j].span->href == href))
        ++j;
    return j - i;
}

void emitSegments(TextSink& sink, const Piece& b, QStringView text,
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
            sink.out += text.mid(segs[i].begin, segs[i].end - segs[i].begin);
            sink.bol = false;
            ++i;
            continue;
        }

        // Строчный комментарий: внутренность буквальна, скобки — структура.
        // Канонические крайние пробелы, как у блочного.
        if (segs[i].span != nullptr && segs[i].span->comment()) {
            sink.out += QLatin1String("<!-- ");
            sink.out += text.mid(segs[i].begin, segs[i].end - segs[i].begin);
            sink.out += QLatin1String(" -->");
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
                for (qsizetype k = segs[i].begin; k < segs[i].end; ++k)
                    (*marksBuf)[size_t(k)] |= kMarkInLink;
            sink.out += QLatin1String("![");
            sink.bol = false;
            appendEscaped(sink, text, segs[i].begin, segs[i].end);
            sink.out += QLatin1String("](");
            appendHref(sink.out, img.href);
            if (!img.title.isEmpty()) {
                // Кавычку и перевод строки разбор в title не пускает; обратная
                // косая экранируется, чтобы не съела закрывающую кавычку.
                sink.out += QLatin1String(" \"");
                for (const QChar tc : img.title) {
                    if (tc == u'\\') sink.out += u'\\';
                    sink.out += tc;
                }
                sink.out += u'"';
            }
            sink.out += u')';
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

        const QString& href = segs[i].span->href;
        const size_t j = i + best;

        const qsizetype gb = segs[i].begin;
        const qsizetype ge = segs[j - 1].end;

        // Содержимое встроенного кода буквально: ни экранирования, ни вложенной
        // разметки. Длина ограничителя подбирается так, чтобы он не встретился
        // внутри, а пробелы-подкладки нужны, когда содержимое само начинается
        // или кончается кавычкой.
        if (attr == kCode) {
            appendCodeSpan(sink.out, text.mid(gb, ge - gb));
            sink.bol = false;
            i = j;
            continue;
        }

        // Адрес выводится дословно, без экранирования: подчёркивания и тильды
        // внутри URL экранировать нельзя, иначе ссылка развалится.
        //
        // И ВНУТРИ ДРУГОЙ РАЗМЕТКИ ТОЖЕ (openMask здесь не спрашивается —
        // решение владельца, 27.08.2026): ссылка, у которой текст и есть адрес,
        // выводится голой или в угловых скобках всюду, где это переживает
        // чтение. Прежде внутри курсива или жирного она превращалась в
        // "[адрес](адрес)" — адрес удваивался, а читать и править такое, особенно
        // когда адрес длинный, невозможно.
        if (attr == kHref && j == i + 1) {
            const LinkShape shape = linkShape(text.mid(gb, ge - gb), href,
                                              ge < text.size() ? text.at(ge) : QChar());
            if (shape != LinkShape::Inline) {
                if (shape == LinkShape::Angle) sink.out += u'<';
                sink.out += text.mid(gb, ge - gb);
                if (shape == LinkShape::Angle) sink.out += u'>';
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
                for (qsizetype k = gb; k < ge; ++k) (*marksBuf)[size_t(k)] |= kMarkInLink;
            } else {
                (*marksBuf)[size_t(gb)] |= kMarkDelimEdge;
                (*marksBuf)[size_t(ge - 1)] |= kMarkDelimEdge;
            }
        }

        const QLatin1String italic = italicDelim(text, gb, ge);
        switch (attr) {
            case kStrike: sink.out += QLatin1String("~~"); break;
            case kBold:   sink.out += QLatin1String("**"); break;
            case kItalic: sink.out += italic; break;
            case kHref:   sink.out += u'['; break;
            default: break;
        }
        sink.bol = false;

        emitSegments(sink, b, text, segs, i, j, openMask | (1u << attr), marksBuf);

        switch (attr) {
            case kStrike: sink.out += QLatin1String("~~"); break;
            case kBold:   sink.out += QLatin1String("**"); break;
            case kItalic: sink.out += italic; break;
            case kHref:
                sink.out += QLatin1String("](");
                appendHref(sink.out, href);
                sink.out += u')';
                break;
            default: break;
        }
        i = j;
    }
}

void appendInlineText(TextSink& sink, const Piece& b) {
    const QStringView text = b.text;
    std::vector<Segment> segs = splitIntoSegments(b);

    // Пометки зависят от расстановки ограничителей, а она — от текста. Поэтому
    // первый проход только собирает пометки, а выводит уже второй.
    std::vector<unsigned char> marks(size_t(text.size()), 0);
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
QString fenceFor(QStringView code, QStringView info) {
    const QChar ch = info.contains(u'`') ? u'~' : u'`';
    qsizetype longest = 0;
    qsizetype run = 0;
    bool atBol = true;
    for (const QChar c : code) {
        if (c == ch && atBol) {
            ++run;
            if (run > longest) longest = run;
            continue;
        }
        run = 0;
        atBol = (c == u'\n');
    }
    const qsizetype n = longest >= 3 ? longest + 1 : 3;
    return QString(n, ch);
}

void validate([[maybe_unused]] const Piece& b) {
    if (b.raw) return;
    assert((b.kind == Kind::Heading) == (b.headingLevel != 0) &&
           "headingLevel only meaningful for a heading");
    assert((b.kind != Kind::Heading || (b.headingLevel >= 1 && b.headingLevel <= 6)) &&
           "heading level outside 1..6");
    assert((!isList(b.kind) || b.level >= 0) && "a list item requires a level");
    // Внутри пункта могут стоять абзац, цитата и код — им уровень осмыслен.
    // Заголовку и пустой строке — нет: заголовка внутри пункта markdown не
    // выражает, а пустая строка ничьей вложенности не имеет.
    assert(((b.kind != Kind::Heading && b.kind != Kind::VSpace && b.kind != Kind::Divider) ||
            b.level == -1) &&
           "this kind takes no level");
    assert((b.kind == Kind::ListItem || !b.checked) && "checked is only meaningful for a task");
    assert((b.kind == Kind::Code || b.info.isEmpty()) && "info is only meaningful for a code block");
    assert(b.level >= -1 && "level shallower than outside a list");
    assert((b.kind != Kind::Html || !b.text.contains(QLatin1String("-->"))) &&
           "comment body cannot contain -->");
    assert((b.kind != Kind::Html || b.runs.empty()) &&
           "no markup inside a comment");
    for ([[maybe_unused]] const Run& s : b.runs) {
        assert((!s.image() || !s.href.isEmpty()) && "an image must have a path");
        assert((!s.comment() ||
                (s.flags == InlineComment && s.href.isEmpty())) &&
               "an inline comment combines with no other markup");
        assert((!s.math() ||
                ((s.flags & ~InlineMathOpen) == InlineMath && s.href.isEmpty())) &&
               "a formula combines with no other markup");
        assert((s.title.isEmpty() || s.image()) && "title is only meaningful for an image");
        assert((!s.image() ||
                (s.flags & (InlineBold | InlineItalic | InlineStrike | InlineCode)) == 0) &&
               "an image combines with no other markup");
    }
}

QString markerFor(const Piece& b, int ordinal) {
    switch (b.marker) {
        case Marker::Bullet:  return QStringLiteral("- ");
        case Marker::Task:    return b.checked ? QStringLiteral("- [x] ") : QStringLiteral("- [ ] ");
        case Marker::Ordered: return QString::number(ordinal) + QLatin1String(". ");
    }
    return {};
}

// Ширина собственно маркера списка. Чекбокс "[ ] " маркером не является — это
// уже содержимое элемента, и вложенный список отсчитывается не от него:
// "- [ ] a" + "  - b" даёт вложенность, а не продолжение текста.
qsizetype markerIndentWidth(const Piece& b, int ordinal) {
    if (b.marker != Marker::Ordered) return 2;
    return QString::number(ordinal).size() + 2;
}

// Отступ блока, стоящего внутри пункта: колонка содержимого того пункта. Вне
// списка отступа нет. Колонки считает сам обход по пунктам, здесь мы их только
// читаем — и осторожно: у оторвавшегося блока колонки может и не оказаться.
qsizetype indentInsideItem(const Piece& b, const std::vector<qsizetype>& contentCol) {
    if (b.level < 0) return 0;
    const size_t at = static_cast<size_t>(b.level) + 1;
    return at < contentCol.size() ? contentCol[at] : 0;
}

// Определение ссылки в дословном куске: "[метка]: /url". Если такое в документе
// есть, то любая пара скобок в тексте может при разборе стать ссылкой, и её
// приходится экранировать.
bool looksLikeLinkDefinition(QStringView raw) {
    qsizetype i = 0;
    while (i < raw.size()) {
        qsizetype e = raw.indexOf(u'\n', i);
        if (e < 0) e = raw.size();
        qsizetype p = i;
        int indent = 0;
        while (p < e && raw.at(p) == u' ' && indent < 4) { ++p; ++indent; }
        if (p < e && raw.at(p) == u'[' && indent < 4) {
            qsizetype q = p + 1;
            while (q < e && raw.at(q) != u']') {
                if (raw.at(q) == u'\\') ++q;
                ++q;
            }
            if (q + 1 < e && raw.at(q) == u']' && raw.at(q + 1) == u':') return true;
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
QString normaliseSpaces(const QString& text) {
    QString out;
    out.reserve(text.size());
    bool inCode = false;    // между заборами блока кода
    const qsizetype n = text.size();
    // Начало содержимого строки: отступ, знаки цитаты, маркер пункта и решётки
    // заголовка — не в счёт. «Ведущий» неразрывный — это ведущий у СОДЕРЖИМОГО:
    // отступ пункта стоит после «- », а не до него. Пока счёт шёл от начала
    // строки, наш собственный отступ после маркера считался мусором, обращался
    // в обычный пробел — и пропадал вовсе при следующем чтении (markdown
    // съедает пробелы после маркера), а с ним расходился круг записи.
    qsizetype content = -1;
    for (qsizetype i = 0; i < n;) {
        if (content < 0) {
            qsizetype end = text.indexOf(u'\n', i);
            if (end < 0) end = n;
            const QStringView line = QStringView(text).mid(i, end - i);
            // Забор блока кода: три знака и больше, с любым отступом перед
            // ними. Внутри блока НЕРАЗРЫВНЫХ НЕ БЫВАЕТ ВОВСЕ — там значим сам
            // пробел, его копируют в терминал, а неразрывный туда попадает
            // только мусором из чужих выгрузок.
            const qsizetype at = i + leadingWhitespace(line);
            if (QStringView(text).mid(at, 3) == QLatin1String("```") ||
                QStringView(text).mid(at, 3) == QLatin1String("~~~"))
                inCode = !inCode;
            content = inCode ? at : i + contentStartOf(line);
        }
        const QChar c = text.at(i);
        if (c == u'\n') {
            out += u'\n';
            content = -1;
            ++i;
            continue;
        }
        if (c == QChar::Nbsp) {
            qsizetype run = 0;
            while (i + run < n && text.at(i + run) == QChar::Nbsp) ++run;
            // Вне кода: ведущие держат отступ, серия из двух и более держит
            // выравнивание, одиночный в середине не значит ничего.
            const bool keep = !inCode && (i <= content || run > 1);
            out += QString(run, keep ? QChar(QChar::Nbsp) : QChar(u' '));
            i += run;
            continue;
        }
        out += c;
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
        out_ += header.toText();
    }

    // Очередной ЛОГИЧЕСКИЙ блок заметки.
    void push(const Piece& b);

    QString finish(std::vector<BlockLines>* map);

protected:

    QString out_;
    // Где начался каждый блок — пока в ЗНАКАХ; в номера строк переведём одним
    // проходом в конце. Так карта не мешает потоку вывода: у него полдюжины
    // мест с `continue`, и считать строки по дороге значило бы не забыть ни
    // одного из них.
    std::vector<qsizetype> startsAt_;
    // Колонка, с которой начинается содержимое на каждом уровне вложенности,
    // и счётчики нумерации.
    std::vector<qsizetype> contentCol_{0};
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

// Строки текста [at, end) без разделителя, одна за другой.
template <typename Fn>
void forEachLine(QStringView body, Fn&& fn) {
    for (qsizetype at = 0; at < body.size();) {
        qsizetype end = body.indexOf(u'\n', at);
        if (end < 0) end = body.size();
        fn(body.mid(at, end - at), at, end < body.size());
        at = end + 1;
    }
}

void Writer::push(const Piece& b) {
    // Имена по-старому: тело переехало в метод целиком, и переименовывать в нём
    // каждую переменную значило бы сделать правку, в которой ошибку не увидеть.
    QString& out = out_;
    std::vector<qsizetype>& startsAt = startsAt_;
    std::vector<qsizetype>& contentCol = contentCol_;
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
        const QStringView body = b.text;

        bool thisIsQuote = !b.raw && b.kind == Kind::Quote;

        // Пустая строка выводится только блоком VSpace — от себя не добавляем
        // ничего. Иначе обязательная пустая строка при следующем чтении стала бы
        // блоком VSpace, которого никто не набирал, и круг бы разошёлся. Что два
        // соседних блока не слипнутся, держит инвариант IR: между такими всегда
        // стоит VSpace (см. Document::wouldMerge).
        // Текст в пустой строке инвариант запрещает, но если он там всё же
        // оказался — печатаем его абзацем. Текст свят; потерять его нельзя ни
        // при каких обстоятельствах.
        if (!b.raw && b.kind == Kind::VSpace && body.isEmpty()) {
            out += u'\n';
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
        if (i > 0 && prevWasQuote && thisIsQuote) out += QLatin1String(">\n");
        // Последний рубеж инварианта: если между блоками нет VSpace, а без
        // пустой строки они слипнутся, — ставим её. Такое IR неправильно, но
        // испортить файл оно не должно.
        else if (i > 0 && wouldMerge(previous_, b))
            out += u'\n';

        if (b.raw) {
            if (b.level >= 0) {
                // Дословный кусок ВНУТРИ ПУНКТА: каждая непустая строка
                // отступает до колонки содержимого пункта — как строки блока
                // кода. Читатель этот отступ снял (newRawInsideItem), здесь он
                // возвращается; пустые строки остаются пустыми.
                const QString pad(indentInsideItem(b, contentCol), u' ');
                forEachLine(body, [&](QStringView line, qsizetype, bool) {
                    if (!line.isEmpty()) {
                        out += pad;
                        out += line;
                    }
                    out += u'\n';
                });
                // Прогон списка кусок внутри пункта не обрывает — как и второй
                // абзац пункта: нумерация за ним продолжается.
                prevWasQuote = false;
                prevLevel = b.level;
                previous_ = b;
                hasFirst_ = true;
                return;
            }
            out += body;
            if (out.isEmpty() || out.back() != u'\n') out += u'\n';
            std::fill(runAlive.begin(), runAlive.end(), 0);
            prevWasQuote = false;
            prevLevel = -1;
            previous_ = b;
            hasFirst_ = true;
            return;
        }

        switch (b.kind) {
            case Kind::Heading: {
                out += QString(b.headingLevel, u'#');
                TextSink sink;
                sink.bol = false;
                sink.hasLinkDefs = hasLinkDefs;
                appendInlineText(sink, b);
                if (!sink.out.isEmpty()) {
                    out += u' ';
                    // Хвостовой прогон '#' Markdown считает закрывающей
                    // последовательностью и выбрасывает.
                    qsizetype hashes = 0;
                    while (hashes < sink.out.size() &&
                           sink.out.at(sink.out.size() - 1 - hashes) == u'#')
                        ++hashes;
                    if (hashes > 0) sink.out.insert(sink.out.size() - hashes, u'\\');
                    out += sink.out;
                }
                out += u'\n';
                break;
            }

            case Kind::Code: {
                // Блок кода внутри пункта отступает до колонки его содержимого —
                // и забор, и каждая строка. Пустые строки внутри кода при этом
                // остаются пустыми: отступ в них дал бы концевые пробелы, а
                // блоку кода они не нужны.
                const QString pad(indentInsideItem(b, contentCol), u' ');
                const QString fence = fenceFor(body, b.info);
                out += pad;
                out += fence;
                out += b.info;
                out += u'\n';
                forEachLine(body, [&](QStringView line, qsizetype, bool) {
                    if (!line.isEmpty()) {
                        out += pad;
                        out += line;
                    }
                    out += u'\n';
                });
                out += pad;
                out += fence;
                out += u'\n';
                break;
            }

            case Kind::Math: {
                // ВЫКЛЮЧНАЯ ФОРМУЛА — ДОСЛОВНО. Текст блока и есть её исходник
                // вместе с долларами: ни разметки, ни экранирования внутри нет,
                // писать нечего, кроме самого текста. Внутри пункта списка
                // отступ до колонки содержимого — как у блока кода.
                // ОТСТУП ТОЛЬКО ПЕРВОЙ СТРОКЕ. Текст блока — дословный
                // исходник, и у строк продолжения СВОИ ведущие пробелы уже
                // внутри него: маркер пункта съел отступ только у первой.
                // Приписав отступ каждой, я удваивал его на каждой записи —
                // `  a &= b` становилось `    a &= b` (поймал набор корпуса).
                const QString pad(indentInsideItem(b, contentCol), u' ');
                bool firstLine = true;
                forEachLine(body, [&](QStringView line, qsizetype, bool) {
                    if (!line.isEmpty()) {
                        if (firstLine) out += pad;
                        out += line;
                    }
                    firstLine = false;
                    out += u'\n';
                });
                break;
            }

            case Kind::VSpace:
            case Kind::Paragraph: {
                // Блок внутри пункта: отступ до колонки его содержимого. Ровно
                // этим markdown и отличает второй абзац пункта от нового блока
                // за списком — маркера у него нет, есть только отступ.
                const qsizetype indent = indentInsideItem(b, contentCol);
                out += QString(indent, u' ');
                TextSink sink;
                sink.contIndent = QString(indent, u' ');
                sink.hasLinkDefs = hasLinkDefs;
                appendInlineText(sink, b);
                out += sink.out;
                out += u'\n';
                break;
            }

            case Kind::Quote: {
                const qsizetype indent = indentInsideItem(b, contentCol);
                out += QString(indent, u' ');
                TextSink sink;
                sink.contIndent = QString(indent, u' ') + QLatin1String("> ");
                sink.hasLinkDefs = hasLinkDefs;
                appendInlineText(sink, b);
                if (sink.out.isEmpty()) {
                    out += u'>';
                } else {
                    out += QLatin1String("> ");
                    out += sink.out;
                }
                out += u'\n';
                break;
            }

            case Kind::Html:
                // Пока единственный вид — комментарий: скобки — структура,
                // текст — внутренность. Крайние пробелы канонические, перенос
                // строки внутри — многострочный комментарий, он законен.
                switch (b.html) {
                    case HtmlKind::Comment: {
                        const qsizetype indent = indentInsideItem(b, contentCol);
                        out += QString(indent, u' ');
                        if (body.isEmpty()) {
                            out += QLatin1String("<!-- -->\n");
                            break;
                        }
                        out += QLatin1String("<!-- ");
                        // Строки внутренности с отступом блока — как строки кода.
                        forEachLine(body, [&](QStringView line, qsizetype at, bool more) {
                            if (at > 0) out += QString(indent, u' ');
                            out += line;
                            if (more) out += u'\n';
                        });
                        out += QLatin1String(" -->\n");
                        break;
                    }
                }
                break;

            case Kind::Divider:
                // Текст свят: разделителю он не положен, но если он там всё же
                // оказался — печатаем абзацем, как это делает пустая строка.
                if (!body.isEmpty()) {
                    TextSink sink;
                    sink.hasLinkDefs = hasLinkDefs;
                    appendInlineText(sink, b);
                    out += sink.out;
                    out += u'\n';
                    break;
                }
                out += QLatin1String("___\n");
                break;

            case Kind::ListItem: {
                assert(b.level <= prevLevel + 1 &&
                       "nesting level skipped: the parser never produces this");
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

                QString marker = markerFor(b, ordinal[level]);
                const qsizetype indent = contentCol[level];
                const qsizetype childIndent = indent + markerIndentWidth(b, ordinal[level]);
                contentCol[level + 1] = childIndent;

                out += QString(indent, u' ');
                if (body.isEmpty()) {
                    // "- " с висящим пробелом Markdown бы съел, но глазами это
                    // читается как мусор.
                    while (!marker.isEmpty() && marker.back() == u' ') marker.chop(1);
                    out += marker;
                } else {
                    out += marker;
                    TextSink sink;
                    sink.contIndent = QString(childIndent, u' ');
                    sink.hasLinkDefs = hasLinkDefs;
                    sink.taskBoxAhead = b.marker == Marker::Bullet;
                    appendInlineText(sink, b);
                    out += sink.out;
                }
                out += u'\n';
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

QString Writer::finish(std::vector<BlockLines>* map) {
    QString& out = out_;
    const std::vector<qsizetype>& startsAt = startsAt_;
    if (map != nullptr) {
        map->assign(startsAt.size(), BlockLines{});
        // Смещения не убывают, поэтому строки считаются одним проходом по
        // выводу: идём по нему, отмечая границы блоков там, где они попались.
        qsizetype at = 0;
        int line = 0;
        std::vector<int> lineAt(startsAt.size() + 1, 0);
        for (size_t k = 0; k < startsAt.size(); ++k) {
            while (at < startsAt[k]) {
                if (out.at(at) == u'\n') ++line;
                ++at;
            }
            lineAt[k] = line;
        }
        while (at < out.size()) {
            if (out.at(at) == u'\n') ++line;
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
// Логический блок заметки — не то же, что QTextBlock: дословные куски лежат в
// документе ПОСТРОЧНО, по блоку на строку, и склеиваются здесь обратно. Признак
// продолжения обязателен: без него разрезанный кусок из двух строк неотличим от
// двух кусков подряд, а это разный markdown. Блок кода — один QTextBlock.

namespace {

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

// Один QTextBlock: и текст, и куски с начертанием. Смещение копится в единицах
// UTF-16 — куски идут подряд и покрывают блок целиком.
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
        if (format.objectType() == FormulaObject) {
            // Формула отдаёт свой исходник целиком: разметки внутри неё нет, и
            // куском строки он не помечается — род блока (Kind::Math) говорит
            // всё сам.
            piece.text += format.property(ObjectSourceProperty).toString();
            continue;
        }
        if (format.objectType() == TableObject) {
            // Таблица — дословный кусок: исходник целиком, признак таблицы
            // едет с ним (сборщику: собрать объект, а не литерал).
            piece.text += format.property(ObjectSourceProperty).toString();
            piece.table = true;
            continue;
        }
        if (format.objectType() == InlineFormulaObject) {
            // Строчная формула отдаёт исходник НА КАЖДЫЙ ЗНАК фрагмента:
            // соседние одинаковые формулы Qt складывает в один фрагмент из
            // двух U+FFFC, и один исходник на фрагмент терял бы вторую.
            const QString source = format.property(ObjectSourceProperty).toString();
            for (qsizetype n = 0; n < text.size(); ++n) {
                Run run;
                run.start = int32_t(piece.text.size());
                piece.text += source;
                run.end = int32_t(piece.text.size());
                run.flags = InlineMath;
                if (withRuns) piece.runs.push_back(std::move(run));
            }
            continue;
        }
        if (format.objectType() == ImageObject) {
            const int32_t at = int32_t(piece.text.size());
            const QString alt = format.property(ObjectAltProperty).toString();
            const bool wiki = !format.hasProperty(ObjectAltProperty);
            piece.text += wiki ? format.property(ObjectSourceProperty).toString() : alt;
            if (!withRuns || wiki) continue;
            Run run;
            run.start = at;
            run.end = int32_t(piece.text.size());
            run.flags = InlineImage;
            run.href = format.anchorHref();
            run.title = format.property(SpanTitleProperty).toString();
            piece.runs.push_back(std::move(run));
            continue;
        }

        if (text.isEmpty()) continue;

        const int32_t offset = int32_t(piece.text.size());
        piece.text += text;
        if (!withRuns) continue;

        const int style = format.intProperty(SpanStyleProperty);
        const QString href = format.anchorHref();
        if (style == 0 && href.isEmpty()) continue;

        Run run;
        run.start = offset;
        run.end = offset + int32_t(text.size());
        run.set(InlineBold, (style & SpanBold) != 0);
        run.set(InlineItalic, (style & SpanItalic) != 0);
        run.set(InlineStrike, (style & SpanStrike) != 0);
        run.set(InlineCode, (style & SpanCode) != 0);
        run.href = href;

        // Подпись картинки плоская по построению: правки могли домешать в
        // формат другие биты — здесь они гасятся, иначе вышло бы то, что файл
        // выразить не может. Картинка без пути — не картинка.
        run.set(InlineImage, (style & SpanImage) != 0 && !run.href.isEmpty());
        if (run.image()) {
            run.flags = InlineImage;
            run.title = format.property(SpanTitleProperty).toString();
        }

        // Строчный комментарий плоский так же; внутренность с "-->" файл
        // выразить не может — такой кусок перестаёт быть комментарием и
        // становится видимым текстом (писатель его экранирует).
        run.set(InlineComment, (style & SpanComment) != 0 && !run.image() &&
                                   !text.contains(QLatin1String("-->")));
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
            // Раскрытая на правку — бит едет с куском: сборщик оставит её
            // текстом, а не свернёт обратно в объект под руками человека.
            if ((style & SpanMathOpen) != 0) run.flags |= InlineMathOpen;
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
// РАСКРЫТУЮ (mathOpen) не трогаем: пока формула раскрыта, человек в ней
// ПЕЧАТАЕТ, и промежуточные состояния законно не формулы — судит их только
// закрытие (closeInlineFormula), а не каждый обход.
void settleMath(Piece& piece) {
    for (Run& run : piece.runs) {
        if (!run.math() || run.mathOpen()) continue;
        if (!wholeMath(piece.view(run))) run.flags = 0;
    }
}

// Комментарий держит свой инвариант на границе документ→файл: разметки внутри
// не бывает, а внутренность с "-->" файл выразить не может — такой блок
// перестаёт быть комментарием и становится видимым текстом.
void settleComment(Piece& piece) {
    if (piece.raw || piece.kind != Kind::Html) return;
    if (piece.text.contains(QLatin1String("-->")))
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

        // Блок документа == блок заметки: дословные куски и код лежат одним
        // QTextBlock, их строки восстанавливает gatherLine из U+2028.
        close();
        open = true;
        piece.raw = raw;
        // Уровень есть и у дословного куска внутри пункта.
        piece.level = levelOf(block);
        if (!raw) {
            piece.kind = kindOf(block);
            if (piece.kind == Kind::Heading) piece.headingLevel = format.headingLevel();
            if (isList(piece.kind)) {
                const MarkerStyle style = markerOf(block);
                piece.marker = style.marker;
                piece.checked = style.checked;
            }
            if (piece.kind == Kind::Code)
                piece.info = format.stringProperty(InfoProperty);
        }
        // Разметку внутри блока кода не читаем: содержимое там буквальное.
        gatherLine(block, piece, !raw && piece.kind != Kind::Code);

        // Признак стоит на последней строке блока — там, где перевод и был.
        //
        // Кладём его И В ТЕКСТ, И В ПРИЗНАК. В текст — писателю: он печатает
        // байты и про признак не знает. В признак — сборщику: обход обязан
        // быть точной обратной стороной разбора, иначе блоки, снятые с живого
        // документа и положенные обратно, теряли бы этот перевод строки, и
        // пустая строка в конце блока кода исчезала бы при каждой операции.
        if (format.boolProperty(TrailingNewlineProperty)) {
            piece.text += u'\n';
            piece.trailingNewline = true;
        }
    }
    close();
}

namespace {

// Каким каноном писать: тем, что уйдёт в файл, или тем, что сейчас в документе.
enum class Canon { Live, File };

// Единственное место, где живая заметка превращается в текст. Ходит прямо по
// внутреннему QTextDocument — ни промежуточного представления, ни второй живой
// модели. Текст, а не байты: в байты он переводится один раз, на границе файла
// (toMarkdown), а буферу обмена и разности байты не нужны вовсе.
//
// ДВА КАНОНА ЗДЕСЬ РАЗЛИЧАЮТСЯ ФЛАГОМ, А НЕ ДВУМЯ ПИСАТЕЛЯМИ. Разница между
// ними одна и вся в ступени documentForFile: живой документ вправе держать то,
// чего markdown не хранит, файл — нет.
//
//   Canon::File — то, что уйдёт в файл. Хвостовой пробел под кареткой срезан,
//     ведущий стал неразрывным, пустые блоки по краям сняты, начертание с
//     пробелом на краю поджато, разметка, которая не читается обратно, снята.
//     Этим живут toMarkdown/fileBytes/saveTo, отпечаток и сравнение тел.
//   Canon::Live — то, что СЕЙЧАС в документе, знак в знак. Этим живёт режим
//     исходника: номер блока в его карте строк — это номер QTextBlock, и на
//     этом стоит точечное наложение правленого текста (document_source.cpp).
//     Им же наборы операций смотрят на живое строение.
QString writeInto(const QTextDocument& doc, const NoteHeader& header, Canon canon,
                  std::vector<BlockLines>* map) {

    // Есть ли в заметке ссылочные определения — от этого зависит экранирование
    // квадратных скобок. Спрашивается ДО записи, потому что ответ нужен уже на
    // первом блоке. Дословный кусок лежит одним блоком, его строки — U+2028;
    // спрашиваем текст таким, каким он лежит в файле (sourceTextOf), иначе
    // вторая и дальнейшие строки куска не увиделись бы вовсе.
    bool hasLinkDefs = false;
    for (QTextBlock b = doc.begin(); b.isValid() && !hasLinkDefs; b = b.next())
        if (isRawBlock(b) && looksLikeLinkDefinition(sourceTextOf(b))) hasLinkDefs = true;

    Writer writer(header, hasLinkDefs, map != nullptr);
    if (canon == Canon::Live) {
        walkPieces(doc, [&](const Piece& piece) {
            writer.push(piece);
            return true;
        });
        return writer.finish(map);
    }

    // Файловый канон: между обходом и писателем встаёт приведение к выразимому.
    // Оно смотрит на документ целиком (пустые строки по краям, слипшиеся стыки,
    // оторвавшиеся от пункта блоки), поэтому блоки собираются списком — ровно
    // так же, как их собирает запись на диск.
    std::vector<Piece> blocks;
    walkPieces(doc, [&](const Piece& piece) {
        blocks.push_back(piece);
        return true;
    });
    for (const Piece& piece : documentForFile(std::move(blocks))) writer.push(piece);
    return writer.finish(map);
}

}  // namespace

// Блоки, заметкой ещё не ставшие, — кусок в буфере обмена, сторона сравнения,
// проба разметки на выживание. Кладём их в документ-однодневку и записываем тем
// же писателем: правил записи двух не бывает, а собрать и обойти кусок
// выделения стоит микросекунды.
//
// КАНОН ЗДЕСЬ ЖИВОЙ, И ЭТО НЕ НЕДОСМОТР. Блоки сюда приходят уже приведёнными
// (documentForFile зовёт writePieces изнутри — withMarkupThatSurvives пробует
// на них разметку), и звать приведение второй раз значило бы уйти в бесконечную
// рекурсию. Тем, кому нужны байты файла, отвечает ZDocument::toMarkdown.
QString writePieces(const std::vector<Piece>& blocks, const NoteHeader& header,
                    std::vector<BlockLines>* map) {
    QTextDocument temp;
    buildDocument(blocks, temp);
    return writeInto(temp, header, Canon::Live, map);
}

QString ZDocument::toMarkdownText() const {
    return writeInto(d_->text, NoteHeader{}, Canon::File, nullptr);
}

std::string ZDocument::toMarkdown() const {
    // ГРАНИЦА ФАЙЛА: единственный перевод текста в байты на пути записи.
    const QByteArray bytes = writeInto(d_->text, NoteHeader{}, Canon::File, nullptr).toUtf8();
    return std::string(bytes.constData(), size_t(bytes.size()));
}

std::string ZDocument::toMarkdown(const NoteHeader& envelope) const {
    const QByteArray bytes = writeInto(d_->text, envelope, Canon::File, nullptr).toUtf8();
    return std::string(bytes.constData(), size_t(bytes.size()));
}

QString ZDocument::toMarkdownText(const NoteHeader& envelope) const {
    return writeInto(d_->text, envelope, Canon::File, nullptr);
}

QString ZDocument::liveMarkdown() const {
    return writeInto(d_->text, NoteHeader{}, Canon::Live, nullptr);
}

QString ZDocument::liveMarkdownWithMap(std::vector<BlockLines>* map) const {
    // Тело БЕЗ шапки: в ней живёт `modified`, она меняется при каждой записи, и
    // всякая разность начиналась бы с неё — всегда одной и той же строки.
    return writeInto(d_->text, NoteHeader{}, Canon::Live, map);
}

std::vector<SourceLine> ZDocument::sourceLines() const {
    std::vector<BlockLines> map;
    const QString whole = liveMarkdownWithMap(&map);
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
