#include "document_saver.h"

#include "doc_model.h"

#include "document_reader.h"
#include "json_dump.h"
#include "parser.h"
#include "serializer.h"

#include <QFileInfo>
#include <QDateTime>
#include <QFile>
#include <QSaveFile>

#include <algorithm>
#include <vector>

namespace zametti {
namespace {

QByteArray toBytes(const std::string& text) {
    return QByteArray(text.data(), static_cast<qsizetype>(text.size()));
}

// Содержимое файла целиком. Заметки маленькие, и побайтовое сравнение и точнее
// хеша, и короче: не надо рассуждать о коллизиях. Читаем именно файл, а не
// помним последнюю запись, — тогда правка снаружи не приводит к «уже сохранено».
QByteArray fileContents(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return file.readAll();
}

bool writeFile(const QString& path, const QByteArray& data, QString* error) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error != nullptr) *error = file.errorString();
        return false;
    }
    const bool ok = file.write(data) == data.size() && file.flush();
    if (!ok && error != nullptr) *error = file.errorString();
    file.close();
    return ok;
}

}  // namespace

QString rescueTimestamp() {
    return QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"));
}

namespace {

bool isSpace(char c) { return c == ' ' || c == '\t'; }

// Начинается ли в этом месте пробельный знак и сколько он занимает байт. Ноль —
// не пробельный. Неразрывный пробел занимает два байта, и рубить его пополам
// нельзя.
int whitespaceAt(std::string_view text, size_t at) {
    if (at >= text.size()) return 0;
    const unsigned char c = static_cast<unsigned char>(text[at]);
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') return 1;
    if (c == 0xC2 && at + 1 < text.size() &&
        static_cast<unsigned char>(text[at + 1]) == 0xA0)
        return 2;
    return 0;
}

int whitespaceBefore(std::string_view text, size_t at) {
    if (at == 0) return 0;
    const unsigned char c = static_cast<unsigned char>(text[at - 1]);
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') return 1;
    if (c == 0xA0 && at >= 2 && static_cast<unsigned char>(text[at - 2]) == 0xC2) return 2;
    return 0;
}

// Буква или цифра. Многобайтовые знаки считаем буквами целиком: для нашей
// задачи важно лишь, слово это или граница слова.
bool wordByte(unsigned char c) {
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           c >= 0x80;
}

// Зачёркивание живёт только на целых словах. Проверено на ядре: "фру~~кты~~"
// разбирается обратно буквальными тильдами, а "фру ~~кты~~" и "abc,~~def~~"
// проходят — рвётся оно ровно тогда, когда сосед снаружи буква или цифра.
// Жирный и курсив внутри слова работают прекрасно, это особенность именно
// тильд.
//
// Поэтому кусок раздаётся наружу до границ слова. Обрезать его внутрь было бы
// хуже: выделив половину слова, человек остался бы вовсе без зачёркивания.
Block withStrikeOnWholeWords(Document& doc, Block block) {
    if (block.raw || block.kind == Kind::Code) return block;

    const std::string_view text = doc.text(block);
    for (Inline& span : doc.inlines(block)) {
        if (!span.strike()) continue;
        size_t from = size_t(qBound(0, int(span.text.start), int(text.size())));
        size_t to = size_t(qBound(int(from), int(span.text.end), int(text.size())));
        while (from > 0 && wordByte(static_cast<unsigned char>(text[from - 1]))) --from;
        while (to < text.size() && wordByte(static_cast<unsigned char>(text[to]))) ++to;
        span.text = {int32_t(from), int32_t(to)};
    }
    return block;
}

// Разметка не может начинаться или кончаться пробелом: markdown такое просто не
// выражает. Знак подчёркивания или звёздочка перед пробелом не открывают
// начертание, и разобранное обратно расходится с документом.
//
// Проверено на ядре: курсив по слову проходит круг, курсив с пробелом на краю —
// нет, и неразрывный пробел ничем не лучше обычного. Перенос строки внутри
// разметки, наоборот, живёт прекрасно.
//
// Поэтому края разметки поджимаются внутрь. Выделить курсивом стих вместе с его
// отступами человек может, а markdown этого не хранит — начертание достанется
// самим строкам, без ведущих пробелов.
// Делят ли эти два спана хоть одно начертание. Если делят, то на их стыке
// разметка не кончается — она продолжается дальше, и стык внутри неё.
bool sharesStyle(const Document& doc, const Inline& a, const Inline& b) {
    return (a.bold() && b.bold()) || (a.italic() && b.italic()) ||
           (a.strike() && b.strike()) || (a.code() && b.code()) ||
           (!a.href.empty() && doc.view(a.href) == doc.view(b.href));
}

// Спаны блока после правки смещений: выбросить схлопнувшиеся, оставшиеся
// подтянуть к началу диапазона. Диапазон обязан оставаться сплошным, поэтому
// не erase, а уплотнение на месте; освободившиеся ячейки остаются в spans
// ничьими — для транзиентного IR это норм.
void compactSpans(Document& doc, Block& block) {
    const std::span<Inline> spans = doc.inlines(block);
    size_t write = 0;
    for (size_t i = 0; i < spans.size(); ++i)
        if (spans[i].text.size() > 0) spans[write++] = spans[i];
    block.inlines.end = block.inlines.start + int32_t(write);
}

// Текст блока укоротили — спаны, вылезшие за его конец, выбрасываем. Вывод от
// этого не меняется: сериализатор такие спаны и так пропускал, а инвариант
// «спан внутри текста своего блока» остаётся целым.
void dropSpansPastText(Document& doc, Block& block) {
    const int32_t size = block.text.size();
    for (Inline& span : doc.inlines(block))
        if (span.text.end > size) span.text = Range{};
    compactSpans(doc, block);
}

Block withTrimmedSpans(Document& doc, Block block) {
    if (block.raw || block.kind == Kind::Code) return block;

    const std::string_view text = doc.text(block);
    const std::span<Inline> spans = doc.inlines(block);
    for (size_t i = 0; i < spans.size(); ++i) {
        Inline& span = spans[i];
        // Пробелов на краю не терпит только начертание: звёздочка или тильда
        // перед пробелом кусок не открывает. Встроенный код и ссылка терпят —
        // проверено на ядре, `[x] ` и [ так ](/url) проходят круг дословно.
        //
        // У куска кода край всегда обратная кавычка, а не пробел, поэтому
        // начертание вокруг него тоже цело.
        if (span.code() || !(span.bold() || span.italic() || span.strike())) continue;
        // Край, к которому вплотную примыкает спан того же начертания, краем
        // разметки не является: жирный кусок со встроенным кодом внутри лежит у
        // нас двумя спанами, и пробел между ними — середина жирного, а не его
        // конец. Поджав такой край, мы разрывали жирный надвое, и открытие файла
        // переписывало его без единой правки.
        const bool joinedLeft = i > 0 && spans[i - 1].text.end == span.text.start &&
                                sharesStyle(doc, spans[i - 1], span);
        const bool joinedRight = i + 1 < spans.size() &&
                                 span.text.end == spans[i + 1].text.start &&
                                 sharesStyle(doc, span, spans[i + 1]);

        size_t from = size_t(qBound(0, int(span.text.start), int(text.size())));
        size_t to = size_t(qBound(int(from), int(span.text.end), int(text.size())));
        while (!joinedLeft && from < to) {
            const int width = whitespaceAt(text, from);
            if (width == 0) break;
            from += size_t(width);
        }
        while (!joinedRight && to > from) {
            const int width = whitespaceBefore(text, to);
            if (width == 0) break;
            to -= size_t(width);
        }
        span.text = {int32_t(from), int32_t(to)};
    }
    compactSpans(doc, block);
    return block;
}

// Заголовок в одну строку. Перенос строки в заголовке markdown не выражает:
// разбор возвращает заголовок и отдельный абзац за ним. Заголовок по природе
// однострочен, поэтому перенос становится пробелом — байт в байт, и смещения
// разметки не съезжают.
Block withHeadingOnOneLine(Document& doc, Block block) {
    if (block.raw || block.kind != Kind::Heading) return block;
    const std::string_view text = doc.text(block);
    if (text.find('\n') == std::string_view::npos) return block;
    // Арена растёт только в хвост: текст не правится на месте, а дописывается
    // заново. Длина та же, поэтому смещения спанов не меняются.
    std::string flat(text);
    for (char& c : flat)
        if (c == '\n') c = ' ';
    block.text = doc.append(flat);
    return block;
}

// Встроенный код через перенос строки markdown тоже не выражает: разбор
// превращает перенос в пробел, и текст расходится с документом. Дотянуть Ctrl+E
// до соседней строки человек может запросто, поэтому такой кусок режется
// построчно — по куску кода на строку.
Block withCodeSpansPerLine(Document& doc, Block block) {
    if (block.raw || block.kind == Kind::Code) return block;

    const size_t textSize = doc.text(block).size();
    const int32_t base = int32_t(doc.spans.size());
    // Читаем спаны по индексу и копией: doc.spans тут же растёт, и вид на него
    // протух бы посреди обхода.
    for (int32_t at = block.inlines.start; at < block.inlines.end; ++at) {
        const Inline span = doc.spans[size_t(at)];
        if (!span.code()) {
            doc.spans.push_back(span);
            continue;
        }
        const size_t end = size_t(qBound(0, int(span.text.end), int(textSize)));
        size_t from = size_t(qBound(0, int(span.text.start), int(end)));
        while (from < end) {
            const size_t found = doc.text(block).find('\n', from);
            const size_t stop = (found == std::string_view::npos || found > end) ? end : found;
            if (stop > from) {
                Inline piece = span;
                piece.text = {int32_t(from), int32_t(stop)};
                doc.spans.push_back(piece);
            }
            from = stop >= end ? end : stop + 1;
        }
    }
    block.inlines = {base, int32_t(doc.spans.size())};
    return block;
}

}  // namespace

bool sameSkeleton(const Document& a, const Document& b) {
    // Метаданные — строка в строку: потерять parent при записи так же нельзя,
    // как потерять текст.
    if (a.meta.present != b.meta.present || a.meta.lines != b.meta.lines) return false;
    const std::vector<Block>& x = a.blocks;
    const std::vector<Block>& y = b.blocks;
    if (x.size() != y.size()) return false;
    for (size_t i = 0; i < x.size(); ++i) {
        if (x[i].raw != y[i].raw) return false;
        if (x[i].raw) {
            if (a.text(x[i]) != b.text(y[i])) return false;
            continue;
        }
        if (x[i].kind != y[i].kind || x[i].level != y[i].level ||
            x[i].marker != y[i].marker || x[i].checked != y[i].checked ||
            x[i].headingLevel != y[i].headingLevel ||
            a.info(x[i]) != b.info(y[i]) || a.text(x[i]) != b.text(y[i]))
            return false;
    }
    return true;
}

// Неразрывный пробел в UTF-8. Именно им сохраняются отступы: обычный пробел в
// начале строки markdown съедает, а этот — нет.
//
// Совет писать сущность "&nbsp;" не годится: ядро отдаёт её буквальным текстом,
// и в заметке было бы видно "&nbsp;" вместо отступа. Прямой знак проходит круг
// целиком — проверено, включая схему из трёх строк.
const char* const kNbsp = "\xC2\xA0";

// Края строк. Ведущие пробелы становятся неразрывными — отступ значим, им
// рисуют схемы и лесенки. Концевые выбрасываются: они как ведущие нули,
// незначащие, а markdown их всё равно съедает.
//
// Строка из одних пробелов считается пустой: несколько раз нажатый пробел на
// пустой строке — не отступ, и оставлять от него неразрывные знаки незачем.
//
// Литеральные блоки не трогаем: в коде и дословных кусках пробел и так значим.
Block withEdgesNormalised(Document& doc, Block block) {
    // В коде пробел значим — его копируют и вставляют в терминал, и хитрым
    // знакам там взяться неоткуда. Трогаем только завершающий перевод строки:
    // забор всё равно ставится с новой строки, и без него разбор вернул бы
    // текст с переводом, а самопроверка честно не дала бы записать.
    if (block.kind == Kind::Code && !block.raw) {
        const std::string_view code = doc.text(block);
        if (!code.empty() && code.back() != '\n') {
            // Дописать байт к чужому куску арены нельзя — он там не последний.
            // Значит, текст переезжает в хвост целиком, с переводом на конце.
            std::string fixed(code);
            fixed.push_back('\n');
            block.text = doc.append(fixed);
        }
        return block;
    }
    if (block.raw) return block;

    const std::string_view text = doc.text(block);
    std::vector<int> map(text.size() + 1, 0);
    std::string out;

    // Именно признаком, а не пустотой out: строка бывает пустой и сама, и по
    // пустоте не отличить «первую строку» от «десятой, но пока пустой». На этом
    // сходились в одну все ведущие пустые строки абзаца.
    bool firstLine = true;
    size_t line = 0;
    for (;;) {
        size_t end = text.find('\n', line);
        const bool last = end == std::string::npos;
        if (last) end = text.size();

        size_t start = line;
        while (start < end && isSpace(text[start])) ++start;
        size_t stop = end;
        while (stop > start && isSpace(text[stop - 1])) --stop;

        // Пустая строка внутри блока — содержимое: в заметках ею отбивают куски
        // текста, и терять её нельзя.
        //
        // В абзаце она сохраняется как есть: абзац потом режется по таким
        // строкам на отдельные блоки, и каждая пустая строка становится
        // настоящей пустой строкой файла. В цитате резать нельзя — две цитаты
        // через пустую строку это уже две цитаты, — и там пустая строка
        // по-прежнему держится неразрывным пробелом.
        //
        // В пункте списка её не держим вовсе: пустой пункт — не отбивка, а след
        // только что нажатого Enter, и невидимый знак ему ни к чему.
        const bool keepBlank = block.kind == Kind::Paragraph || block.kind == Kind::Quote;
        const bool blank = start >= stop;
        if (!blank || keepBlank) {
            if (!firstLine) out.push_back('\n');
            firstLine = false;
            if (blank && block.kind == Kind::Quote) out += kNbsp;
        }

        // Ведущие пробелы: каждый становится неразрывным. Отступ значим, им
        // рисуют схемы и лесенки.
        for (size_t k = line; k < start; ++k) {
            map[k] = int(out.size());
            if (!blank) out += kNbsp;
        }
        for (size_t k = start; k < stop; ++k) {
            map[k] = int(out.size());
            out.push_back(text[k]);
        }
        for (size_t k = stop; k <= end && k < text.size(); ++k) map[k] = int(out.size());

        if (last) {
            map[text.size()] = int(out.size());
            break;
        }
        line = end + 1;
    }

    for (Inline& span : doc.inlines(block)) {
        const size_t from = size_t(qBound(0, int(span.text.start), int(text.size())));
        const size_t to = size_t(qBound(0, int(span.text.end), int(text.size())));
        span.text = {map[from], map[to]};
    }
    compactSpans(doc, block);
    block.text = doc.append(out);
    return block;
}

// Последняя оговорка про разметку — и самая важная. Правил о том, где знаки
// начертания открывают и закрывают кусок, в markdown много: тильды не работают
// внутри слова, звёздочки вокруг одной точки не работают вовсе, а знаки,
// стоящие в самом тексте, путаются с разметкой. Повторять их все у себя —
// значит переписать половину спецификации и всё равно ошибиться.
//
// Поэтому правило простое: текст свят, разметка — по возможности. Если блок с
// разметкой обратно не читается, разметка снимается, а текст остаётся до знака.
// Потерять начертание неприятно; потерять слово нельзя.
Block withMarkupThatSurvives(Document& doc, Block block) {
    if (block.raw || block.inlines.empty()) return block;

    // Уровень вложенности сбрасываем: вопрос здесь только про разметку внутри
    // строки, а сериализатор в одиночном блоке ждёт, что уровень не прыгает
    // через один, и на вложенном пункте падал бы проверкой.
    //
    // Блок уезжает в чужой документ — значит, только через adopt: Range нашей
    // арены в чужой указывал бы в произвольное место.
    Document one;
    Block probe = one.adopt(doc, block);
    probe.level = isList(probe.kind) ? 0 : -1;
    one.blocks.push_back(probe);
    const Document back = parse(serialize(one));
    if (back.blocks.size() == 1 && !back.blocks[0].raw &&
        back.text(back.blocks[0]) == doc.text(block))
        return block;

    block.inlines = Range{};
    return block;
}

// Дословный кусок выводится как есть, и завершающий перевод строки для него —
// часть текста. Правка внутри такого блока его снимает, и разбор возвращает
// текст с переводом, которого в документе нет.
Block withRawNewline(Document& doc, Block block) {
    if (!block.raw || block.text.empty()) return block;
    const std::string_view raw = doc.text(block);
    if (raw.back() == '\n') return block;
    std::string fixed(raw);
    fixed.push_back('\n');
    block.text = doc.append(fixed);
    return block;
}

// Пустой вложенный пункт markdown не выражает вовсе. Одинокий "-" под текстом
// родителя читается подчёркиванием заголовка, и весь список уезжает в дословный
// кусок — ровно от этого сорвалось сохранение на живой заметке.
//
// Замер на ядре: ни звёздочка, ни плюс, ни цифра не спасают — пункт либо ломает
// список, либо просто исчезает при разборе. На верхнем уровне такой пункт
// прекрасно записывается, а пустая вложенная ЗАДАЧА проходит и подавно: "- [ ]"
// одиноким дефисом уже не выглядит.
//
// Поэтому выбрасываем только пустой вложенный буллет или номер, а его потомков
// поднимаем на уровень — иначе они остались бы без родителя.
std::vector<Block> withoutEmptyNested(std::vector<Block> doc) {
    std::vector<Block> out;
    out.reserve(doc.size());
    for (size_t i = 0; i < doc.size(); ++i) {
        const Block& block = doc[i];
        const bool drop = !block.raw && block.text.empty() && block.level > 0 &&
                          block.kind == Kind::ListItem && block.marker != Marker::Task;
        if (!drop) {
            out.push_back(block);
            continue;
        }
        // Потомки — всё, что глубже, до первого блока своего уровня или выше.
        for (size_t k = i + 1; k < doc.size(); ++k) {
            Block& next = doc[k];
            if (next.raw || !isList(next.kind) || next.level <= block.level) break;
            --next.level;
        }
    }
    return out;
}

// Абзац режется по пустым строкам на отдельные блоки, и каждая пустая строка
// становится блоком VSpace — то есть настоящей пустой строкой файла. Круг при
// этом сходится точно: "первая\n\nвторая" читается обратно ровно этими же тремя
// блоками.
//
// Только абзац. Пункт списка так резать нельзя — у второй половины появился бы
// маркер, которого никто не ставил; цитату тоже — две цитаты через пустую
// строку это уже две цитаты. Там пустая строка держится неразрывным пробелом.
void appendSplitOnBlankLines(std::vector<Block>& out, Document& doc, Block block) {
    if (block.raw) {
        // Дословный кусок, начинающийся с пустой строки: сама она куском не
        // является — разбор вернул бы её отдельной пустой строкой перед ним.
        size_t at = 0;
        const std::string_view raw = doc.text(block);
        while (at < raw.size() && raw[at] == '\n') {
            Block gap;
            gap.kind = Kind::VSpace;
            out.push_back(gap);
            ++at;
        }
        // Отрезать начало — это подвинуть границу Range, копировать нечего.
        block.text.start += int32_t(at);
        if (!block.text.empty()) out.push_back(block);
        return;
    }
    if (block.kind != Kind::Paragraph) {
        out.push_back(block);
        return;
    }

    const int32_t base = block.text.start;
    const size_t textSize = doc.text(block).size();
    size_t at = 0;
    size_t pieceFrom = std::string_view::npos;
    auto flush = [&](size_t to) {
        if (pieceFrom == std::string_view::npos) return;
        Block piece;
        piece.kind = Kind::Paragraph;
        // Уровень переносим: куски остаются там же, где стоял сам абзац, — то
        // есть внутри своего пункта, если он там стоял.
        piece.level = block.level;
        // Кусок абзаца — подотрезок его же байтов: копировать текст не нужно.
        piece.text = {base + int32_t(pieceFrom), base + int32_t(to)};
        const int32_t spanBase = int32_t(doc.spans.size());
        for (int32_t i = block.inlines.start; i < block.inlines.end; ++i) {
            const Inline span = doc.spans[size_t(i)];
            const size_t from = std::max(size_t(span.text.start), pieceFrom);
            const size_t stop = std::min(size_t(span.text.end), to);
            if (stop <= from) continue;
            Inline moved = span;
            moved.text = {int32_t(from - pieceFrom), int32_t(stop - pieceFrom)};
            doc.spans.push_back(moved);
        }
        piece.inlines = {spanBase, int32_t(doc.spans.size())};
        out.push_back(piece);
        pieceFrom = std::string_view::npos;
    };

    for (;;) {
        size_t end = doc.text(block).find('\n', at);
        const bool last = end == std::string_view::npos;
        if (last) end = textSize;

        if (end == at) {                       // пустая строка
            flush(at > 0 ? at - 1 : at);
            Block gap;
            gap.kind = Kind::VSpace;
            out.push_back(gap);
        } else if (pieceFrom == std::string_view::npos) {
            pieceFrom = at;
        }

        if (last) {
            flush(textSize);
            break;
        }
        at = end + 1;
    }
}

Document documentForFile(Document doc) {
    std::vector<Block> out;
    out.reserve(doc.blocks.size());
    for (Block& block : doc.blocks) {
        // Пробельная пустая строка (каретка ещё не ушла с неё) — пустая:
        // markdown пробелы выбросил бы сам, а edges превратили бы их в nbsp.
        if (!block.raw && block.kind == Kind::VSpace &&
            doc.text(block).find_first_not_of(" \t") == std::string_view::npos)
            block.text = Range{};
        appendSplitOnBlankLines(
            out, doc,
            withMarkupThatSurvives(
                doc, withStrikeOnWholeWords(
                         doc, withTrimmedSpans(
                                  doc, withCodeSpansPerLine(
                                           doc, withHeadingOnOneLine(
                                                    doc, withRawNewline(
                                                             doc, withEdgesNormalised(
                                                                      doc, block))))))));
    }

    // Пустые строки в начале документа файл выразить не может: пустая строка
    // там стоит между блоками, а до первого блока никакого стыка нет — разбор
    // такие строки просто пропускает. Снимаем их сами, иначе круг разошёлся бы.
    size_t head = 0;
    while (head < out.size() && !out[head].raw && out[head].kind == Kind::VSpace) ++head;
    if (head > 0) out.erase(out.begin(), out.begin() + qsizetype(head));

    // В конце документа пустые строки не нужны по той же причине: после
    // последнего блока стыка тоже нет.
    while (!out.empty() && !out.back().raw &&
           (out.back().kind == Kind::VSpace ||
            (out.back().kind == Kind::Paragraph && out.back().text.empty())))
        out.pop_back();
    if (!out.empty()) {
        Block& last = out.back();
        if (!last.raw && last.kind != Kind::Code) {
            // Хвост отрезается сдвигом границы Range: байты остаются в арене,
            // блок просто перестаёт на них смотреть.
            const auto tail = [&](size_t width) {
                return doc.view({last.text.end - int32_t(width), last.text.end});
            };
            while (last.text.size() >= 1 && tail(1) == "\n") last.text.end -= 1;
            // Хвостовые неразрывные строки последнего блока — тот же случай:
            // пустая строка, которой в файле после последнего блока не бывает.
            const std::string_view nbsp = kNbsp;
            while (last.text.size() >= int32_t(nbsp.size()) + 1 &&
                   tail(nbsp.size()) == nbsp &&
                   tail(nbsp.size() + 1).substr(0, 1) == "\n") {
                last.text.end -= int32_t(nbsp.size()) + 1;
            }
            if (doc.text(last) == nbsp) last.text = Range{};
            dropSpansPastText(doc, last);
        }
        if (!out.back().raw && out.back().kind == Kind::Paragraph && out.back().text.empty())
            out.pop_back();
    }

    // Блок, оторвавшийся от своего пункта, — обычный абзац. Отступ такого блока
    // файл прочтёт продолжением пункта, которого больше нет, и круг разойдётся.
    // Оторваться он может от чего угодно: пункт вырезали, вставили кусок из
    // другого места, поправили файл снаружи.
    {
        int deepest = -1;   // уровень последнего пункта или его продолжения
        for (Block& block : out) {
            // Дословный кусок выводится с нулевой колонки и список этим
            // заканчивает: всё, что за ним, стоит уже снаружи.
            if (block.raw) { deepest = -1; continue; }
            if (block.kind == Kind::VSpace) continue;
            if (isList(block.kind)) {
                // Пункт может открыть только один уровень за раз. Глубже —
                // значит его родителя больше нет: прижимаем к возможному.
                if (block.level > deepest + 1) block.level = deepest + 1;
                deepest = block.level;
                continue;
            }
            if (block.level < 0) { deepest = -1; continue; }
            if (deepest < 0) block.level = -1;
            else if (block.level > deepest) block.level = deepest;
            else deepest = block.level;
        }
    }

    // Последний рубеж инварианта: между блоками, которые в файле слиплись бы,
    // обязана стоять пустая строка. Операции держат это правило сами, но здесь
    // мы отвечаем за файл — а испорченный файл дороже лишней проверки.
    std::vector<Block> spaced;
    spaced.reserve(out.size() + 2);
    for (const Block& block : out) {
        if (!spaced.empty() && doc.wouldMerge(spaced.back(), block)) {
            Block gap;
            gap.kind = Kind::VSpace;
            spaced.push_back(gap);
        }
        spaced.push_back(block);
    }

    // Метаданные проезжают насквозь как есть: нормализация — про блоки.
    doc.blocks = withoutEmptyNested(std::move(spaced));
    return doc;
}

namespace {

std::string_view asView(const QByteArray& bytes) {
    return std::string_view(bytes.constData(), static_cast<size_t>(bytes.size()));
}

}  // namespace

bool canonicaliseNoteFile(const QString& path, std::string& text, Digest& digest) {
    const Document parsed = parse(text);
    if (!parsed.meta.present) return false;   // не наша заметка

    const std::string canonical = serialize(parsed);
    if (canonical == text) return false;      // и так канон

    // Последний рубеж, тот же, что и при записи: причёсанное обязано читаться
    // обратно тем же документом. Не сошлось — файл не трогаем вовсе.
    if (!sameSkeleton(parsed, parse(canonical))) return false;

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) return false;
    file.write(canonical.data(), qint64(canonical.size()));
    if (!file.commit()) return false;

    text = canonical;
    digest = hashOf(text);
    return true;
}

SaveOutcome saveDocument(const QTextDocument& doc, const QString& path,
                         const QString& timestamp, DocumentReaderFn reader,
                         const NoteMeta& meta, const Digest& known) {
    Document read = reader ? reader(doc) : readDocument(doc);
    read.meta = meta;
    const Document ir = documentForFile(std::move(read));
    const QByteArray text = toBytes(serialize(ir));

    // Не писать, если не изменилось.
    const Digest digest = hashOf(asView(text));
    if (!known.empty()) {
        // Отпечаток файла известен — сравниваем отпечатки, файл не читаем.
        if (digest == known) return {SaveResult::Unchanged, {}, {}, {}, false, digest, text};
    } else if (QFile::exists(path) && fileContents(path) == text) {
        // Не знаем — читаем и сравниваем байты, как раньше.
        return {SaveResult::Unchanged, {}, {}, {}, false, digest, text};
    }

    // Последний рубеж: то, что мы собрались записать, должно читаться обратно в
    // тот же документ.
    //
    // Строго сверяются строение и текст: число блоков, род, уровень, язык,
    // содержимое. Разметка внутри строки может оказаться богаче — голую ссылку
    // человек набирает текстом, а файл читает её ссылкой, и не дать этого
    // записать значило бы запретить писать ссылки. Байт при этом не теряется:
    // текст блока обязан совпасть до знака.
    const Document reread =
        parse(std::string(text.constData(), static_cast<size_t>(text.size())));
    if (!sameSkeleton(ir, reread)) {
        // В хранилище побитое складывается в .rescue/ (не синхронизируется);
        // вне хранилища — рядом с файлом, как раньше.
        const QFileInfo fileInfo(path);
        const QString rescueDir = fileInfo.absolutePath() + QStringLiteral("/.rescue");
        const QString rescuePath =
            QFileInfo(fileInfo.absolutePath() + QStringLiteral("/.zametti")).isDir() &&
                    QFileInfo(rescueDir).isDir()
                ? rescueDir + QLatin1Char('/') + fileInfo.fileName() +
                      QStringLiteral(".rescue-") + timestamp
                : path + QStringLiteral(".rescue-") + timestamp;
        QString error;
        if (!writeFile(rescuePath, text, &error)) {
            return {SaveResult::Failed,
                    QStringLiteral("самопроверка не прошла, и аварийный файл не записан: ") +
                        error,
                    {}, {}, false, {}, {}};
        }
        return {SaveResult::Rescued,
                QStringLiteral("самопроверка перед записью не прошла: разобранное обратно "
                               "не совпало с документом. Файл не тронут, буфер сохранён в ") +
                    rescuePath,
                rescuePath, {}, false, {}, {}};
    }

    // Замена файла целиком и разом: QSaveFile пишет во временный файл рядом и
    // переименовывает его на commit. Оборванная запись не оставит половину
    // заметки на месте целой.
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        return {SaveResult::Failed,
                QStringLiteral("не открыть на запись: ") + file.errorString(), {}, {},
                false, {}, {}};
    }
    file.write(text);
    if (!file.commit()) {
        return {SaveResult::Failed, QStringLiteral("не записать: ") + file.errorString(), {},
                {}, false, {}, {}};
    }
    // Отпечаток — по тому же буферу и только после самопроверки: не прошла
    // она — файл не тронут, и отпечатку взяться неоткуда.
    return {SaveResult::Written, {}, {}, reread, toJson(reread) != toJson(ir), digest, text};
}

}  // namespace zametti
