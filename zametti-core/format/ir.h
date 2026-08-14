// Промежуточное представление документа.
//
// Транзиентное: живёт только на время загрузки и сохранения. Рядом с
// QTextDocument никогда не существует, синхронизировать его не с чем.
//
// Ядро работает в UTF-8 (std::string). Ни одного include из Qt здесь быть не должно.
//
// --- представление ---------------------------------------------------------
//
// Всё содержимое документа лежит в одной сплошной арене байтов
// (Document::chars): текст блоков, дословные куски, info-строки блоков кода,
// адреса и заголовки ссылок и картинок. Блок и спан — POD со смещениями, ни
// один из них ничем не владеет и ничего не аллоцирует. Копия Document — это
// memcpy четырёх буферов, и она полностью независима от оригинала.
//
// Координатных пространств три, и путать их нельзя:
//
//   Block::text, Block::info, Inline::href, Inline::title — БАЙТЫ в chars;
//   Block::inlines                                        — ИНДЕКСЫ в spans;
//   Inline::text                       — БАЙТЫ ОТ НАЧАЛА ТЕКСТА СВОЕГО БЛОКА.
//
// Все три проверяются в validate().
//
// Спан меряется от текста блока, а не от начала арены, нарочно. При правке
// текст блока переезжает в хвост арены целиком (replace-by-append), и
// абсолютные смещения спанов пришлось бы сдвигать при каждой такой правке.
// Забытый сдвиг был бы невидим: старые байты из арены никуда не делись,
// смещение осталось бы в границах chars, и validate() промолчал бы, а разметка
// молча приехала бы от прошлой версии текста. С относительным смещением этого
// класса ошибки нет.
//
// Арена растёт только в хвост. Смещения не протухают никогда, поэтому правка
// содержимого — это replace-by-append: новые байты дописываются в конец, Range
// перенаправляется, старые байты остаются мусором. Для транзиентного IR это
// нормально.
//
// Порядок блоков в blocks и порядок байтов в арене между собой не связаны:
// нулевой блок вполне может смотреть в самый хвост арены (так бывает после
// adopt и после любой правки). Никто не ходит по арене подряд, и полагаться на
// её порядок нельзя.
//
// Возвращаемые string_view живут не дольше самого Document и портятся при
// любой дописи в арену: держать их через вызовы, которые арену трогают,
// нельзя.

#ifndef ZAMETTI_IR_H
#define ZAMETTI_IR_H

#include "block_kind.h"

#include "../doc/note_header.h"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace zametti {


// Полуоткрытый диапазон [start, end), как cv::Range. int32 достаточно: заметка
// больше двух гигабайт в модель мира не входит, и validate() это утверждает.
struct Range {
    int32_t start = 0;
    int32_t end = 0;

    bool empty() const { return start >= end; }
    int32_t size() const { return end - start; }
};

inline bool operator==(Range a, Range b) { return a.start == b.start && a.end == b.end; }
inline bool operator!=(Range a, Range b) { return !(a == b); }

// Оси блока (Kind, Marker, HtmlKind) переехали в doc/block_kind.h: они
// описывают семантику заметки, а не устройство этого представления.


// Кусок строки с одним начертанием (бывший Span; имя — из словаря CommonMark).
struct Inline {
    // Байты ОТ НАЧАЛА ТЕКСТА СВОЕГО БЛОКА, а не от начала арены: текст блока
    // при правке переезжает целиком, и спанам от этого меняться незачем.
    Range   text;
    Range   href;           // байты в chars; пусто → не ссылка, при картинке — путь
    Range   title;          // байты в chars; пусто → нет; осмыслен только при картинке
    uint8_t flags = 0;

    bool bold() const    { return (flags & InlineBold) != 0; }
    bool italic() const  { return (flags & InlineItalic) != 0; }
    bool strike() const  { return (flags & InlineStrike) != 0; }
    bool code() const    { return (flags & InlineCode) != 0; }
    bool image() const   { return (flags & InlineImage) != 0; }
    bool comment() const { return (flags & InlineComment) != 0; }
    bool math() const    { return (flags & InlineMath) != 0; }
    // Выключная формула стоит отдельным абзацем и обрамлена двумя долларами.
    // Признак выводится из самого текста: держать его отдельным флагом значит
    // однажды разойтись с содержимым.
    bool displayMath(std::string_view spanText) const {
        return math() && spanText.size() >= 4 && spanText.compare(0, 2, "$$") == 0;
    }

    void set(InlineFlag flag, bool on) {
        flags = on ? uint8_t(flags | flag) : uint8_t(flags & ~unsigned(flag));
    }
};

struct Block {
    Kind   kind         = Kind::Paragraph;
    Marker marker       = Marker::Bullet;    // осмысленно только при Kind::ListItem
    bool   checked      = false;             // осмысленно только при Marker::Task
    // true → text это дословные байты, а kind, marker, info и разметка
    // игнорируются. Отдельного буфера под дословное нет: арена одна.
    bool   raw          = false;
    int8_t headingLevel = 0;                 // осмысленно только при Kind::Heading
    HtmlKind html       = HtmlKind::Comment; // осмысленно только при Kind::Html
    // На каком уровне списка стоит блок. -1 — снаружи списка.
    //
    // Ось общая, а не поле пункта: пункт всегда имеет уровень, но и другие
    // блоки могут стоять внутри пункта — второй абзац, код, цитата. Уровень и
    // говорит, внутри какого пункта они стоят.
    int16_t level       = -1;

    Range  text;      // байты в Document::chars: чистый текст либо дословный кусок
    Range  info;      // байты в Document::chars; осмысленна только при Kind::Code
    Range  inlines;   // ИНДЕКСЫ в Document::spans, не байты
};

// Нумерованный ли это пункт и задача ли это. Спрашивать про род тут нечего: род
// у всех пунктов один, различает их маркер. Арены эти вопросы не касаются.
inline bool isOrdered(const Block& b) {
    return !b.raw && b.kind == Kind::ListItem && b.marker == Marker::Ordered;
}
inline bool isTask(const Block& b) {
    return !b.raw && b.kind == Kind::ListItem && b.marker == Marker::Task;
}

// Шапка заметки живёт своим классом (doc/note_header.h). Здесь она только
// поле: представление умирает, а шапка остаётся.
struct Document {
    NoteHeader meta;
    std::string chars;             // единая арена: текст, дословное, info, href, title
    std::vector<Inline> spans;
    std::vector<Block> blocks;

    // --- доступ ------------------------------------------------------------

    std::string_view view(Range r) const {
        return std::string_view(chars).substr(size_t(r.start), size_t(r.size() > 0 ? r.size() : 0));
    }
    std::string_view text(const Block& b) const { return view(b.text); }
    std::string_view info(const Block& b) const { return view(b.info); }
    // Текст спана меряется от текста блока — потому блок и обязателен.
    std::string_view text(const Block& b, const Inline& s) const {
        return text(b).substr(size_t(s.text.start), size_t(s.text.size() > 0 ? s.text.size() : 0));
    }
    std::string_view href(const Inline& s) const { return view(s.href); }
    std::string_view title(const Inline& s) const { return view(s.title); }

    std::span<const Inline> inlines(const Block& b) const {
        return std::span<const Inline>(spans.data() + b.inlines.start, size_t(b.inlines.size()));
    }
    std::span<Inline> inlines(const Block& b) {
        return std::span<Inline>(spans.data() + b.inlines.start, size_t(b.inlines.size()));
    }

    // --- построение --------------------------------------------------------

    // Дописать байты в хвост арены. Возвращённый Range не протухает никогда.
    Range append(std::string_view bytes);
    // Дописать спаны в хвост spans. Возвращённый Range — индексы, а не байты.
    Range appendInlines(std::span<const Inline> items);

    // Новый блок с текстом: байты уезжают в арену, блок возвращается значением.
    // Куда его класть — дело вызывающего (push_back, insert, присвоение).
    Block newBlock(Kind kind, std::string_view text = {});
    // Дословный кусок. Дословное всегда кончается переводом строки: без него
    // IR последнего блока не совпал бы сам с собой после круга.
    Block newRaw(std::string_view bytes);

    // Блок из ЧУЖОГО документа. Его байты (текст, info, а у каждого спана —
    // текст, href, title) копируются в хвост нашей арены, спаны — в хвост
    // нашего spans, и возвращённый блок смотрит уже на них. Единственный
    // законный способ перенести блок между документами: Range чужого документа
    // в нашей арене указывает в произвольное место.
    Block adopt(const Document& from, const Block& b);

    // --- запросы, которым нужна арена --------------------------------------

    // Дословный кусок — законченный HTML-комментарий: начинается с "<!--" и
    // кончается строкой с "-->" на конце. HTML-блок этого типа по CommonMark
    // кончается ровно на первой строке с "-->", поэтому такой кусок — один
    // целый комментарий, и внутри него не прячется ничего незакрытого. Куски с
    // "-->" в середине (обычные HTML-блоки) сюда не попадают — и не должны:
    // замерено, что "<div>" с "-->" внутри жадно съедает соседний код при
    // следующем чтении.
    bool isClosedHtmlComment(const Block& b) const;

    // Слипнутся ли эти два блока, если поставить их в файле подряд без пустой
    // строки. Проверено на ядре: абзац после абзаца читается одним абзацем,
    // абзац после пункта и после цитаты — их ленивым продолжением. Всё прочее —
    // заголовок, список, код, цитата после абзаца — прекрасно стоит вплотную.
    //
    // Отсюда инвариант IR: между такими блоками обязан стоять VSpace. Тогда
    // сериализатору не нужно вставлять пустую строку от себя, и одна пустая
    // строка в файле — это ровно один блок VSpace, в обе стороны.
    bool wouldMerge(const Block& previous, const Block& next) const;

    // Оба координатных пространства целы. Только ассерты: в NDEBUG пусто.
    void validate() const;
};

}  // namespace zametti

#endif  // ZAMETTI_IR_H
