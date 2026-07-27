#include "document_saver.h"

#include "doc_model.h"

#include "document_reader.h"
#include "json_dump.h"
#include "parser.h"
#include "serializer.h"

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
int whitespaceAt(const std::string& text, size_t at) {
    if (at >= text.size()) return 0;
    const unsigned char c = static_cast<unsigned char>(text[at]);
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') return 1;
    if (c == 0xC2 && at + 1 < text.size() &&
        static_cast<unsigned char>(text[at + 1]) == 0xA0)
        return 2;
    return 0;
}

int whitespaceBefore(const std::string& text, size_t at) {
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
Block withStrikeOnWholeWords(Block block) {
    if (!block.rawSource.empty() || block.kind == Kind::Code) return block;

    for (Span& span : block.inlines) {
        if (!span.strike) continue;
        size_t from = size_t(qBound(0, span.offset, int(block.text.size())));
        size_t to = size_t(qBound(int(from), span.offset + span.length,
                                  int(block.text.size())));
        while (from > 0 && wordByte(static_cast<unsigned char>(block.text[from - 1]))) --from;
        while (to < block.text.size() &&
               wordByte(static_cast<unsigned char>(block.text[to])))
            ++to;
        span.offset = int(from);
        span.length = int(to - from);
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
Block withTrimmedSpans(Block block) {
    if (!block.rawSource.empty() || block.kind == Kind::Code) return block;

    for (Span& span : block.inlines) {
        size_t from = size_t(qBound(0, span.offset, int(block.text.size())));
        size_t to = size_t(qBound(int(from), span.offset + span.length,
                                  int(block.text.size())));
        while (from < to) {
            const int width = whitespaceAt(block.text, from);
            if (width == 0) break;
            from += size_t(width);
        }
        while (to > from) {
            const int width = whitespaceBefore(block.text, to);
            if (width == 0) break;
            to -= size_t(width);
        }
        span.offset = int(from);
        span.length = int(to - from);
    }
    block.inlines.erase(std::remove_if(block.inlines.begin(), block.inlines.end(),
                                       [](const Span& s) { return s.length <= 0; }),
                        block.inlines.end());
    return block;
}

// Заголовок в одну строку. Перенос строки в заголовке markdown не выражает:
// разбор возвращает заголовок и отдельный абзац за ним. Заголовок по природе
// однострочен, поэтому перенос становится пробелом — байт в байт, и смещения
// разметки не съезжают.
Block withHeadingOnOneLine(Block block) {
    if (!block.rawSource.empty() || block.kind != Kind::Heading) return block;
    for (char& c : block.text)
        if (c == '\n') c = ' ';
    return block;
}

// Встроенный код через перенос строки markdown тоже не выражает: разбор
// превращает перенос в пробел, и текст расходится с документом. Дотянуть Ctrl+E
// до соседней строки человек может запросто, поэтому такой кусок режется
// построчно — по куску кода на строку.
Block withCodeSpansPerLine(Block block) {
    if (!block.rawSource.empty() || block.kind == Kind::Code) return block;

    std::vector<Span> pieces;
    for (const Span& span : block.inlines) {
        if (!span.code) {
            pieces.push_back(span);
            continue;
        }
        const size_t end = size_t(qBound(0, span.offset + span.length,
                                         int(block.text.size())));
        size_t from = size_t(qBound(0, span.offset, int(end)));
        while (from < end) {
            const size_t found = block.text.find('\n', from);
            const size_t stop = (found == std::string::npos || found > end) ? end : found;
            if (stop > from) {
                Span piece = span;
                piece.offset = int(from);
                piece.length = int(stop - from);
                pieces.push_back(piece);
            }
            from = stop >= end ? end : stop + 1;
        }
    }
    block.inlines = std::move(pieces);
    return block;
}

}  // namespace

bool sameSkeleton(const Document& a, const Document& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].rawSource != b[i].rawSource) return false;
        if (!a[i].rawSource.empty()) continue;
        if (a[i].kind != b[i].kind || a[i].level != b[i].level ||
            a[i].headingLevel != b[i].headingLevel || a[i].info != b[i].info ||
            a[i].text != b[i].text)
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
Block withEdgesNormalised(Block block) {
    // В коде пробел значим — его копируют и вставляют в терминал, и хитрым
    // знакам там взяться неоткуда. Трогаем только завершающий перевод строки:
    // забор всё равно ставится с новой строки, и без него разбор вернул бы
    // текст с переводом, а самопроверка честно не дала бы записать.
    if (block.kind == Kind::Code && block.rawSource.empty()) {
        if (!block.text.empty() && block.text.back() != '\n') block.text.push_back('\n');
        return block;
    }
    if (!block.rawSource.empty()) return block;

    const std::string& text = block.text;
    std::vector<int> map(text.size() + 1, 0);
    std::string out;

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
        // текста, и терять её нельзя. В файле она пустой быть не может — пустая
        // строка блок заканчивает, — поэтому в неё ставится неразрывный пробел.
        // Тот же приём, что и с отступами, и по той же причине.
        //
        // Только в обычном тексте и цитате. Пустой пункт списка — не отбивка, а
        // след только что нажатого Enter, и невидимый знак ему ни к чему.
        const bool keepBlank = block.kind == Kind::Paragraph || block.kind == Kind::Quote;
        const bool blank = start >= stop;
        if (!blank || keepBlank) {
            if (!out.empty()) out.push_back('\n');
            if (blank) out += kNbsp;
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

    for (Span& span : block.inlines) {
        const size_t from = size_t(qBound(0, span.offset, int(text.size())));
        const size_t to = size_t(qBound(0, span.offset + span.length, int(text.size())));
        span.offset = map[from];
        span.length = map[to] - map[from];
    }
    block.inlines.erase(std::remove_if(block.inlines.begin(), block.inlines.end(),
                                       [](const Span& s) { return s.length <= 0; }),
                        block.inlines.end());
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
Block withMarkupThatSurvives(Block block) {
    if (!block.rawSource.empty() || block.inlines.empty()) return block;

    const Document one{block};
    const Document back = parse(serialize(one));
    if (back.size() == 1 && back[0].rawSource.empty() && back[0].text == block.text)
        return block;

    block.inlines.clear();
    return block;
}

// Дословный кусок выводится как есть, и завершающий перевод строки для него —
// часть текста. Правка внутри такого блока его снимает, и разбор возвращает
// текст с переводом, которого в документе нет.
Block withRawNewline(Block block) {
    if (block.rawSource.empty()) return block;
    if (block.rawSource.back() != '\n') block.rawSource.push_back('\n');
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
Document withoutEmptyNested(Document doc) {
    Document out;
    out.reserve(doc.size());
    for (size_t i = 0; i < doc.size(); ++i) {
        const Block& block = doc[i];
        const bool drop = block.rawSource.empty() && block.text.empty() &&
                          block.level > 0 &&
                          (block.kind == Kind::Bullet || block.kind == Kind::Ordered);
        if (!drop) {
            out.push_back(block);
            continue;
        }
        // Потомки — всё, что глубже, до первого блока своего уровня или выше.
        for (size_t k = i + 1; k < doc.size(); ++k) {
            Block& next = doc[k];
            if (!next.rawSource.empty() || !isList(next.kind) || next.level <= block.level)
                break;
            --next.level;
        }
    }
    return out;
}

// Пустой абзац markdown выразить нечем: пустая строка в файле — разделитель
// блоков, а не блок. В документе он заводится каждым Enter, и без этой уборки
// самопроверка честно ловила бы расхождение при каждом сохранении.
//
// Пустой пункт списка верхнего уровня при этом остаётся: "-" в файле
// записывается прекрасно.
// Пустой ли это абзац — то есть строка, которой в заметке отбивают куски текста.
// Неразрывный пробел мы ставим в такие строки сами, поэтому он тоже считается
// пустотой.
bool isBlankParagraph(const Block& block) {
    if (!block.rawSource.empty() || block.kind != Kind::Paragraph) return false;
    for (size_t i = 0; i < block.text.size();) {
        const unsigned char c = static_cast<unsigned char>(block.text[i]);
        if (c == 0xC2 && i + 1 < block.text.size() &&
            static_cast<unsigned char>(block.text[i + 1]) == 0xA0) {
            i += 2;
            continue;
        }
        if (c == ' ' || c == '\t' || c == '\n') {
            ++i;
            continue;
        }
        return false;
    }
    return true;
}

Document documentForFile(Document doc) {
    Document out;
    out.reserve(doc.size());
    for (Block& block : doc) {
        Block trimmed = withMarkupThatSurvives(withStrikeOnWholeWords(withTrimmedSpans(
            withCodeSpansPerLine(withHeadingOnOneLine(
                withRawNewline(withEdgesNormalised(std::move(block))))))));
        // Пустой абзац в файле пустым быть не может: пустая строка там —
        // разделитель блоков, а не блок. Ставим в него неразрывный пробел, и
        // отбивка сохраняется — это заметки, и пустые строки в них значимы.
        if (trimmed.rawSource.empty() && trimmed.kind == Kind::Paragraph &&
            trimmed.text.empty())
            trimmed.text = kNbsp;
        out.push_back(std::move(trimmed));
    }

    // Подряд идущие пустые абзацы — это один разделитель, а не десять. Так его
    // высота и выходит предсказуемой: поле сверху, n высот строки, поле снизу.
    // Десятью блоками между строками добавлялись бы ещё девять полей.
    Document merged;
    merged.reserve(out.size());
    for (Block& block : out) {
        if (!merged.empty() && isSeparatorBlock(merged.back()) && isSeparatorBlock(block)) {
            merged.back().text.push_back('\n');
            merged.back().text += block.text;
            continue;
        }
        merged.push_back(std::move(block));
    }
    out = std::move(merged);

    // А вот в конце документа пустые строки не нужны: хвост из них набирается
    // случайно и ничего не отбивает.
    while (!out.empty() && isBlankParagraph(out.back())) out.pop_back();
    if (!out.empty()) {
        Block& last = out.back();
        if (last.rawSource.empty() && last.kind != Kind::Code) {
            while (!last.text.empty() && last.text.back() == '\n') last.text.pop_back();
            // Хвостовые неразрывные строки последнего блока — тот же случай.
            const std::string nbsp = kNbsp;
            while (last.text.size() >= nbsp.size() + 1 &&
                   last.text.compare(last.text.size() - nbsp.size(), nbsp.size(), nbsp) == 0 &&
                   last.text[last.text.size() - nbsp.size() - 1] == '\n') {
                last.text.erase(last.text.size() - nbsp.size() - 1);
            }
            if (last.text == nbsp) last.text.clear();
        }
        if (out.back().rawSource.empty() && out.back().kind == Kind::Paragraph &&
            out.back().text.empty())
            out.pop_back();
    }
    return withoutEmptyNested(std::move(out));
}

SaveOutcome saveDocument(const QTextDocument& doc, const QString& path,
                         const QString& timestamp, DocumentReaderFn reader) {
    const Document ir =
        documentForFile(reader ? reader(doc) : readDocument(doc));
    const QByteArray text = toBytes(serialize(ir));

    if (QFile::exists(path) && fileContents(path) == text)
        return {SaveResult::Unchanged, {}, {}, {}, false};

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
        const QString rescuePath = path + QStringLiteral(".rescue-") + timestamp;
        QString error;
        if (!writeFile(rescuePath, text, &error)) {
            return {SaveResult::Failed,
                    QStringLiteral("самопроверка не прошла, и аварийный файл не записан: ") +
                        error,
                    {}, {}, false};
        }
        return {SaveResult::Rescued,
                QStringLiteral("самопроверка перед записью не прошла: разобранное обратно "
                               "не совпало с документом. Файл не тронут, буфер сохранён в ") +
                    rescuePath,
                rescuePath, {}, false};
    }

    // Замена файла целиком и разом: QSaveFile пишет во временный файл рядом и
    // переименовывает его на commit. Оборванная запись не оставит половину
    // заметки на месте целой.
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        return {SaveResult::Failed,
                QStringLiteral("не открыть на запись: ") + file.errorString(), {}, {}, false};
    }
    file.write(text);
    if (!file.commit()) {
        return {SaveResult::Failed, QStringLiteral("не записать: ") + file.errorString(), {},
                {}, false};
    }
    return {SaveResult::Written, {}, {}, reread, toJson(reread) != toJson(ir)};
}

}  // namespace zametti
