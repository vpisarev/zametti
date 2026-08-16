#include "document_saver.h"

#include "document_impl.h"

#include "history_rules.h"

#include "doc_model.h"

#include "document.h"
#include "document_pieces.h"
#include "serializer.h"

#include <QTextDocument>

#include <QFileInfo>
#include <QDateTime>
#include <QFile>
#include <QSaveFile>

#include <algorithm>
#include <vector>

namespace zametti {
namespace {

// Логические блоки живого документа — местная ступень записи.
std::vector<Piece> piecesOfDocument(const QTextDocument& doc) {
    std::vector<Piece> out;
    walkPieces(doc, [&](const Piece& piece) {
        out.push_back(piece);
        return true;
    });
    return out;
}

// Пустая строка отдельным блоком.
Piece vspacePiece() {
    Piece out;
    out.kind = Kind::VSpace;
    return out;
}

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
Piece withStrikeOnWholeWords(Piece block) {
    if (block.raw || block.kind == Kind::Code) return block;

    const std::string_view text = block.text;
    for (Run& span : block.runs) {
        if (!span.strike()) continue;
        size_t from = size_t(qBound(0, int(span.start), int(text.size())));
        size_t to = size_t(qBound(int(from), int(span.end), int(text.size())));
        while (from > 0 && wordByte(static_cast<unsigned char>(text[from - 1]))) --from;
        while (to < text.size() && wordByte(static_cast<unsigned char>(text[to]))) ++to;
        span.start = int32_t(from);
        span.end = int32_t(to);
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
bool sharesStyle(const Run& a, const Run& b) {
    return (a.bold() && b.bold()) || (a.italic() && b.italic()) ||
           (a.strike() && b.strike()) || (a.code() && b.code()) ||
           (!a.href.empty() && a.href == b.href);
}

// Куски блока после правки смещений: схлопнувшиеся выбрасываем.
void compactRuns(Piece& block) {
    std::vector<Run>& runs = block.runs;
    runs.erase(std::remove_if(runs.begin(), runs.end(),
                              [](const Run& run) { return run.end <= run.start; }),
               runs.end());
}

// Текст блока укоротили — куски, вылезшие за его конец, выбрасываем. Вывод от
// этого не меняется: писатель такие куски и так пропускал, а инвариант «кусок
// внутри текста своего блока» остаётся целым.
void dropRunsPastText(Piece& block) {
    const int32_t size = int32_t(block.text.size());
    for (Run& run : block.runs)
        if (run.end > size) run.end = run.start;
    compactRuns(block);
}

Piece withTrimmedSpans(Piece block) {
    if (block.raw || block.kind == Kind::Code) return block;

    const std::string text = block.text;
    std::vector<Run>& spans = block.runs;
    for (size_t i = 0; i < spans.size(); ++i) {
        Run& span = spans[i];
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
        const bool joinedLeft =
            i > 0 && spans[i - 1].end == span.start && sharesStyle(spans[i - 1], span);
        const bool joinedRight = i + 1 < spans.size() && span.end == spans[i + 1].start &&
                                 sharesStyle(span, spans[i + 1]);

        size_t from = size_t(qBound(0, int(span.start), int(text.size())));
        size_t to = size_t(qBound(int(from), int(span.end), int(text.size())));
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
        span.start = int32_t(from);
        span.end = int32_t(to);
    }
    compactRuns(block);
    return block;
}

// Заголовок в одну строку. Перенос строки в заголовке markdown не выражает:
// разбор возвращает заголовок и отдельный абзац за ним. Заголовок по природе
// однострочен, поэтому перенос становится пробелом — байт в байт, и смещения
// разметки не съезжают.
Piece withHeadingOnOneLine(Piece block) {
    if (block.raw || block.kind != Kind::Heading) return block;
    if (block.text.find('\n') == std::string::npos) return block;
    // Длина та же, поэтому смещения кусков не меняются.
    for (char& c : block.text)
        if (c == '\n') c = ' ';
    return block;
}

// Встроенный код через перенос строки markdown тоже не выражает: разбор
// превращает перенос в пробел, и текст расходится с документом. Дотянуть Ctrl+E
// до соседней строки человек может запросто, поэтому такой кусок режется
// построчно — по куску кода на строку.
Piece withCodeSpansPerLine(Piece block) {
    if (block.raw || block.kind == Kind::Code) return block;

    const size_t textSize = block.text.size();
    std::vector<Run> out;
    out.reserve(block.runs.size());
    for (const Run& span : block.runs) {
        if (!span.code()) {
            out.push_back(span);
            continue;
        }
        const size_t end = size_t(qBound(0, int(span.end), int(textSize)));
        size_t from = size_t(qBound(0, int(span.start), int(end)));
        while (from < end) {
            const size_t found = block.text.find('\n', from);
            const size_t stop = (found == std::string::npos || found > end) ? end : found;
            if (stop > from) {
                Run piece = span;
                piece.start = int32_t(from);
                piece.end = int32_t(stop);
                out.push_back(std::move(piece));
            }
            from = stop >= end ? end : stop + 1;
        }
    }
    block.runs = std::move(out);
    return block;
}

}  // namespace

bool sameSkeleton(const std::vector<Piece>& x, const std::vector<Piece>& y) {
    if (x.size() != y.size()) return false;
    for (size_t i = 0; i < x.size(); ++i) {
        if (x[i].raw != y[i].raw) return false;
        if (x[i].raw) {
            if (x[i].text != y[i].text) return false;
            continue;
        }
        if (x[i].kind != y[i].kind || x[i].level != y[i].level ||
            x[i].marker != y[i].marker || x[i].checked != y[i].checked ||
            x[i].headingLevel != y[i].headingLevel || x[i].info != y[i].info ||
            x[i].text != y[i].text)
            return false;
    }
    return true;
}

// То же, но ВКЛЮЧАЯ разметку внутри строки. Отвечает на другой вопрос: не
// «можно ли писать», а «отличается ли перечитанное от документа хоть чем-то» —
// голую ссылку человек набирает текстом, а файл читает её ссылкой, и виджет
// обязан догнать документ этим содержимым.
bool sameContent(const std::vector<Piece>& x, const std::vector<Piece>& y) {
    if (!sameSkeleton(x, y)) return false;
    for (size_t i = 0; i < x.size(); ++i) {
        if (x[i].raw) continue;
        if (x[i].runs.size() != y[i].runs.size()) return false;
        for (size_t k = 0; k < x[i].runs.size(); ++k) {
            const Run& a = x[i].runs[k];
            const Run& b = y[i].runs[k];
            if (a.start != b.start || a.end != b.end || a.flags != b.flags ||
                a.href != b.href || a.title != b.title)
                return false;
        }
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
Piece withEdgesNormalised(Piece block) {
    // В коде пробел значим — его копируют и вставляют в терминал, и хитрым
    // знакам там взяться неоткуда. Трогаем только завершающий перевод строки:
    // забор всё равно ставится с новой строки, и без него разбор вернул бы
    // текст с переводом, а самопроверка честно не дала бы записать.
    if (block.kind == Kind::Code && !block.raw) {
        if (!block.text.empty() && block.text.back() != '\n') {
            block.text.push_back('\n');
            block.trailingNewline = true;
        }
        return block;
    }
    // ФОРМУЛА ЛИТЕРАЛЬНА, как код: ни ведущие пробелы в неразрывные, ни
    // хвостовые прочь. Это то самое правило, под которое она попадала спаном и
    // из-за которого заметка владельца уехала в файл с U+00A0.
    if (block.kind == Kind::Math) return block;
    if (block.raw) return block;

    const std::string text = block.text;
    std::vector<int> map(text.size() + 1, 0);
    std::string out;

    // ВНУТРИ ФОРМУЛЫ ПРОБЕЛ — ЛИТЕРАЛЬНЫЙ, как в коде. Здесь я и испортил
    // владельцу заметку: правило «ведущие пробелы становятся неразрывными»
    // относится к отступам, которыми рисуют схемы и лесенки, а формула,
    // записанная в несколько строк, попала под него заодно — её строки
    // продолжения уехали в файл с U+00A0. Читается такое всюду (KaTeX и MathJax
    // пробелы юникода игнорируют), но это ПРАВКА ТЕКСТА, которой человек не
    // просил, и в самом latex такие пробелы значат ровно ничего.
    std::vector<std::pair<size_t, size_t>> mathAt;
    for (const Run& span : block.runs)
        if (span.math())
            mathAt.emplace_back(size_t(qMax(0, int(span.start))),
                                size_t(qMax(0, int(span.end))));
    const auto insideMath = [&](size_t at) {
        for (const auto& span : mathAt)
            if (at >= span.first && at < span.second) return true;
        return false;
    };

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
        size_t stop = end;
        if (!insideMath(line) && !insideMath(end > line ? end - 1 : line)) {
            while (start < end && isSpace(text[start])) ++start;
            while (stop > start && isSpace(text[stop - 1])) --stop;
        }

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

    for (Run& span : block.runs) {
        const size_t from = size_t(qBound(0, int(span.start), int(text.size())));
        const size_t to = size_t(qBound(0, int(span.end), int(text.size())));
        span.start = map[from];
        span.end = map[to];
    }
    compactRuns(block);
    block.text = std::move(out);
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
Piece withMarkupThatSurvives(Piece block) {
    if (block.raw || block.runs.empty()) return block;

    // Уровень вложенности сбрасываем: вопрос здесь только про разметку внутри
    // строки, а писатель в одиночном блоке ждёт, что уровень не прыгает через
    // один, и на вложенном пункте падал бы проверкой.
    Piece probe = block;
    probe.level = isList(probe.kind) ? 0 : -1;

    std::vector<Piece> back;
    NoteHeader ignored;
    parsePieces(writePieces({probe}), back, ignored);
    if (back.size() == 1 && !back[0].raw && back[0].text == block.text) return block;

    block.runs.clear();
    return block;
}

// Дословный кусок выводится как есть, и завершающий перевод строки для него —
// часть текста. Правка внутри такого блока его снимает, и разбор возвращает
// текст с переводом, которого в документе нет.
Piece withRawNewline(Piece block) {
    if (!block.raw || block.text.empty()) return block;
    if (block.text.back() == '\n') return block;
    block.text.push_back('\n');
    block.trailingNewline = true;
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
std::vector<Piece> withoutEmptyNested(std::vector<Piece> doc) {
    std::vector<Piece> out;
    out.reserve(doc.size());
    for (size_t i = 0; i < doc.size(); ++i) {
        const Piece& block = doc[i];
        const bool drop = !block.raw && block.text.empty() && block.level > 0 &&
                          block.kind == Kind::ListItem && block.marker != Marker::Task;
        if (!drop) {
            out.push_back(std::move(doc[i]));
            continue;
        }
        // Потомки — всё, что глубже, до первого блока своего уровня или выше.
        for (size_t k = i + 1; k < doc.size(); ++k) {
            Piece& next = doc[k];
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
void appendSplitOnBlankLines(std::vector<Piece>& out, Piece block) {
    if (block.raw) {
        // Дословный кусок, начинающийся с пустой строки: сама она куском не
        // является — разбор вернул бы её отдельной пустой строкой перед ним.
        size_t at = 0;
        while (at < block.text.size() && block.text[at] == '\n') {
            out.push_back(vspacePiece());
            ++at;
        }
        block.text.erase(0, at);
        if (!block.text.empty()) out.push_back(std::move(block));
        return;
    }
    if (block.kind != Kind::Paragraph) {
        out.push_back(std::move(block));
        return;
    }

    const size_t textSize = block.text.size();
    size_t at = 0;
    size_t pieceFrom = std::string::npos;
    auto flush = [&](size_t to) {
        if (pieceFrom == std::string::npos) return;
        Piece piece;
        piece.kind = Kind::Paragraph;
        // Уровень переносим: куски остаются там же, где стоял сам абзац, — то
        // есть внутри своего пункта, если он там стоял.
        piece.level = block.level;
        piece.text = block.text.substr(pieceFrom, to - pieceFrom);
        for (const Run& span : block.runs) {
            const size_t from = std::max(size_t(span.start), pieceFrom);
            const size_t stop = std::min(size_t(span.end), to);
            if (stop <= from) continue;
            Run moved = span;
            moved.start = int32_t(from - pieceFrom);
            moved.end = int32_t(stop - pieceFrom);
            piece.runs.push_back(std::move(moved));
        }
        out.push_back(std::move(piece));
        pieceFrom = std::string::npos;
    };

    for (;;) {
        size_t end = block.text.find('\n', at);
        const bool last = end == std::string::npos;
        if (last) end = textSize;

        if (end == at) {                       // пустая строка
            flush(at > 0 ? at - 1 : at);
            out.push_back(vspacePiece());
        } else if (pieceFrom == std::string::npos) {
            pieceFrom = at;
        }

        if (last) {
            flush(textSize);
            break;
        }
        at = end + 1;
    }
}

std::vector<Piece> documentForFile(std::vector<Piece> doc) {
    std::vector<Piece> out;
    out.reserve(doc.size());
    for (Piece& block : doc) {
        // Пробельная пустая строка (каретка ещё не ушла с неё) — пустая:
        // markdown пробелы выбросил бы сам, а edges превратили бы их в nbsp.
        if (!block.raw && block.kind == Kind::VSpace &&
            block.text.find_first_not_of(" \t") == std::string::npos)
            block.text.clear();
        appendSplitOnBlankLines(
            out, withMarkupThatSurvives(withStrikeOnWholeWords(withTrimmedSpans(
                     withCodeSpansPerLine(withHeadingOnOneLine(
                         withRawNewline(withEdgesNormalised(std::move(block)))))))));
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
        Piece& last = out.back();
        if (!last.raw && last.kind != Kind::Code) {
            const auto tail = [&](size_t width) {
                return std::string_view(last.text).substr(last.text.size() - width, width);
            };
            while (last.text.size() >= 1 && tail(1) == "\n") last.text.pop_back();
            // Хвостовые неразрывные строки последнего блока — тот же случай:
            // пустая строка, которой в файле после последнего блока не бывает.
            const std::string_view nbsp = kNbsp;
            while (last.text.size() >= nbsp.size() + 1 && tail(nbsp.size()) == nbsp &&
                   tail(nbsp.size() + 1).substr(0, 1) == "\n") {
                last.text.resize(last.text.size() - nbsp.size() - 1);
            }
            if (last.text == nbsp) last.text.clear();
            dropRunsPastText(last);
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
        for (Piece& block : out) {
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
    std::vector<Piece> spaced;
    spaced.reserve(out.size() + 2);
    for (Piece& block : out) {
        if (!spaced.empty() &&
            wouldMerge(spaced.back().kind, spaced.back().raw,
                       spaced.back().isClosedHtmlComment(), block.kind, block.raw, block.level))
            spaced.push_back(vspacePiece());
        spaced.push_back(std::move(block));
    }

    return withoutEmptyNested(std::move(spaced));
}

namespace {

std::string_view asView(const QByteArray& bytes) {
    return std::string_view(bytes.constData(), static_cast<size_t>(bytes.size()));
}

}  // namespace

bool canonicaliseNoteFile(const QString& path, std::string& text, Digest& digest) {
    ZDocument note;
    note.loadMarkdown(text);
    if (!note.hasHeader()) return false;   // не наша заметка

    const std::string canonical = note.toMarkdown();
    if (canonical == text) return false;   // и так канон

    // Последний рубеж, тот же, что и при записи: причёсанное обязано читаться
    // обратно тем же документом. Не сошлось — файл не трогаем вовсе.
    ZDocument back;
    back.loadMarkdown(canonical);
    if (!note.sameSkeleton(back)) return false;

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) return false;
    file.write(canonical.data(), qint64(canonical.size()));
    if (!file.commit()) return false;

    text = canonical;
    digest = hashOf(text);
    return true;
}

QByteArray noteBytes(const QTextDocument& doc, const NoteHeader& meta, DocumentReaderFn reader,
                     std::vector<Piece>* fileBlocks) {
    std::vector<Piece> forFile =
        documentForFile(reader ? reader(doc) : piecesOfDocument(doc));
    const QByteArray text = toBytes(writePieces(forFile, meta));
    if (fileBlocks != nullptr) *fileBlocks = std::move(forFile);
    return text;
}

SaveOutcome saveDocument(const QTextDocument& doc, const QString& path,
                         const QString& timestamp, DocumentReaderFn reader,
                         const NoteHeader& meta, const Digest& known,
                         const std::vector<Piece>* prebuiltBlocks,
                         const QByteArray* prebuiltText) {
    const bool ready = prebuiltBlocks != nullptr && prebuiltText != nullptr;
    std::vector<Piece> built;
    if (!ready) built = documentForFile(reader ? reader(doc) : piecesOfDocument(doc));
    const std::vector<Piece>& ir = ready ? *prebuiltBlocks : built;
    const QByteArray text = ready ? *prebuiltText : toBytes(writePieces(ir, meta));

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
    std::vector<Piece> reread;
    NoteHeader rereadHeader;
    parsePieces(asView(text), reread, rereadHeader);
    // РАСХОЖДЕНИЕ БОЛЬШЕ НЕ ЗАПРЕЩАЕТ ЗАПИСЬ.
    //
    // Прежде самопроверка отказывалась писать вовсе: файл оставался прежним, а
    // буфер уезжал в .rescue. Задумано это было как последний рубеж против
    // потери данных, а на деле стало способом её устроить: у владельца отказ
    // повторялся на каждом автосохранении, он выключил предупреждение
    // («больше не беспокоить»), доработал заметку, вышел — и не нашёл ни одной
    // своей правки. Сторож, поставленный беречь текст, его и потерял.
    //
    // Теперь пишем всегда, а расхождение остаётся ДИАГНОСТИКОЙ: копия буфера
    // ложится в .rescue и человеку говорится, что именно не сошлось. Это стало
    // возможно потому, что у заметки есть полная история (журнал со слепками):
    // неудачная запись отменима, а потерянная работа — нет.
    QString rescuePath;
    if (!sameSkeleton(ir, reread)) {
        // В хранилище побитое складывается в .rescue/ (не синхронизируется);
        // вне хранилища — рядом с файлом, как раньше.
        const QFileInfo fileInfo(path);
        const QString rescueDir = fileInfo.absolutePath() + QStringLiteral("/.rescue");
        rescuePath =
            QFileInfo(fileInfo.absolutePath() + QStringLiteral("/.zametti")).isDir() &&
                    QFileInfo(rescueDir).isDir()
                ? rescueDir + QLatin1Char('/') + fileInfo.fileName() +
                      QStringLiteral(".rescue-") + timestamp
                : path + QStringLiteral(".rescue-") + timestamp;
        QString error;
        // Копия — дело полезное, но не обязательное: не легла, и ладно, запись
        // всё равно состоится. Молчать при этом нельзя.
        if (!writeFile(rescuePath, text, &error)) {
            std::fprintf(stderr, "аварийная копия не записана: %s\n",
                         error.toUtf8().constData());
            rescuePath.clear();
        }
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
    // Записано. Если самопроверка не сошлась, говорим об этом — но записью, а
    // не отказом: правки человека уже на диске, а копия буфера лежит рядом.
    QString message;
    if (!rescuePath.isEmpty())
        message = QStringLiteral("самопроверка не сошлась: разобранное обратно отличается от "
                                 "документа. Заметка записана, копия буфера — в ") +
                  rescuePath;
    const bool differs = !sameContent(reread, ir);
    return {SaveResult::Written, message, rescuePath, std::move(reread), differs, digest, text};
}

// ЗАМЕТКА ПОДАЁТ ПИСАТЕЛЮ СВОЙ ДОКУМЕНТ САМА. Путь записи один и живёт выше;
// метод нужен затем, чтобы ради записи не приходилось отдавать наружу живой
// QTextDocument — а он не отдаётся никому и никогда.
SaveOutcome ZDocument::saveTo(const QString& path, const QString& timestamp,
                              DocumentReaderFn reader, const NoteHeader& meta,
                              const Digest& known, const std::vector<Piece>* prebuiltBlocks,
                              const QByteArray* prebuiltText) {
    return saveDocument(d_->text, path, timestamp, std::move(reader), meta, known,
                        prebuiltBlocks, prebuiltText);
}

}  // namespace zametti
