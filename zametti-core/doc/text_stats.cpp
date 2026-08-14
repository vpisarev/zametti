#include "text_stats.h"

#include "doc_model.h"

#include <QChar>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>

#include <algorithm>
#include <array>
#include <cstdint>

namespace zametti {
namespace {

// Что знак делает со словом. Три ответа, а не два: знак может слово
// продолжать, разрывать — или не значить ничего (комбинирующая метка).
enum class Role : uint8_t {
    Separator,   // разрывает слово
    Letter,      // буква или цифра — слово идёт
    Extender,    // ни то ни другое: статус не меняет
};

Role roleFromCategory(char32_t cp);

// Начало таблиц Юникода — своей таблицей. Дотуда достают все языки, на которых
// владелец пишет заметки: латиница, кириллица целиком (включая расширения),
// греческий, иврит, арабский, армянский.
//
// ЧЕСТНО ПРО ЦЕНУ: ускорения замер не показал. Счёт слов по 239 КБ русского
// UTF-8 — 1039 мкс с таблицей на 4096 входов и 1043 мкс с таблицей на 128, то
// есть в пределах шума; узкое место не в QChar::category, а в самом разборе
// UTF-8. Таблица оставлена решением владельца, и стоит она четыре килобайта.
//
// Таблица заполняется ИЗ Qt, а не написана руками: разойтись с ним она тогда не
// может по построению, и ошибка вида «забыл, что U+0483 — комбинирующая метка»
// невозможна. Четыре тысячи вызовов на старте — это микросекунды.
constexpr char32_t kFastLimit = 4096;

// Заполняется до main, а не при первом обращении: у статики внутри функции есть
// сторож инициализации, и его проверка легла бы на каждый знак текста. Ничего,
// кроме таблиц Юникода самого Qt, инициализация не трогает, и звать её до
// QGuiApplication можно. Раньше неё в этом файле не считает никто.
const std::array<Role, kFastLimit> kFastRoles = [] {
    std::array<Role, kFastLimit> roles{};
    for (char32_t c = 0; c < kFastLimit; ++c) roles[size_t(c)] = roleFromCategory(c);
    return roles;
}();

Role roleFromCategory(char32_t cp) {
    switch (QChar::category(cp)) {
        case QChar::Letter_Uppercase:
        case QChar::Letter_Lowercase:
        case QChar::Letter_Titlecase:
        case QChar::Letter_Modifier:
        case QChar::Letter_Other:
        case QChar::Number_DecimalDigit:
        case QChar::Number_Letter:
        case QChar::Number_Other:
            return Role::Letter;
        // Комбинирующие метки прилипают к предыдущей букве, знаки класса Cf
        // (мягкий перенос, соединители эмодзи) невидимы вовсе: ни то ни другое
        // на экране само по себе не стоит и слова не разрывает.
        case QChar::Mark_NonSpacing:
        case QChar::Mark_SpacingCombining:
        case QChar::Mark_Enclosing:
        case QChar::Other_Format:
            return Role::Extender;
        default:
            return Role::Separator;
    }
}

// Один вызов в таблицы Юникода на знак, а не два: и «буква ли», и «метка ли»
// читаются из одной категории.
inline Role roleOf(char32_t cp) {
    return cp < kFastLimit ? kFastRoles[size_t(cp)] : roleFromCategory(cp);
}

}  // namespace

int countWords(QStringView text) {
    int words = 0;
    bool insideWord = false;
    const QChar* const begin = text.constData();
    const QChar* const end = begin + text.size();
    for (const QChar* at = begin; at != end; ++at) {
        // Обычный текст — кириллица, латиница, знаки препинания — живёт ниже
        // суррогатов, и разбирать пару там незачем.
        char32_t cp = at->unicode();
        if (at->isHighSurrogate() && at + 1 != end && (at + 1)->isLowSurrogate()) {
            cp = QChar::surrogateToUcs4(*at, *(at + 1));
            ++at;
        }
        const Role role = roleOf(cp);
        if (role == Role::Letter) {
            insideWord = true;
        } else if (role == Role::Separator) {
            words += int(insideWord);
            insideWord = false;
        }
    }
    return words + int(insideWord);   // слово, кончившееся вместе с текстом
}

int countWords(std::string_view utf8) {
    int words = 0;
    bool insideWord = false;
    const unsigned char* at = reinterpret_cast<const unsigned char*>(utf8.data());
    const unsigned char* const end = at + utf8.size();
    while (at != end) {
        char32_t cp = *at;
        if (cp < 0x80) {
            ++at;
        } else {
            // Разбор UTF-8. Битый байт — сам себе знак-разделитель: текст,
            // который до нас дошёл, уже проверен разбором, а падать счётчику
            // слов на чужом байте всё равно незачем.
            int extra = 0;
            if ((cp & 0xE0) == 0xC0) { cp &= 0x1F; extra = 1; }
            else if ((cp & 0xF0) == 0xE0) { cp &= 0x0F; extra = 2; }
            else if ((cp & 0xF8) == 0xF0) { cp &= 0x07; extra = 3; }
            else { cp = 0xFFFD; }
            ++at;
            for (int i = 0; i < extra; ++i) {
                if (at == end || (*at & 0xC0) != 0x80) { cp = 0xFFFD; break; }
                cp = (cp << 6) | char32_t(*at & 0x3F);
                ++at;
            }
        }
        const Role role = roleOf(cp);
        if (role == Role::Letter) {
            insideWord = true;
        } else if (role == Role::Separator) {
            words += int(insideWord);
            insideWord = false;
        }
    }
    return words + int(insideWord);
}

int countLineBreaks(QStringView text) {
    int breaks = 0;
    for (QChar unit : text)
        if (unit == QChar::LineSeparator) ++breaks;
    return breaks;
}

BlockStats blockStats(const QTextBlock& block) {
    BlockStats out;
    if (!block.isValid()) return out;
    const QString text = block.text();
    out.breaks = countLineBreaks(text);
    // Фотография занимает блок целиком, а её текст — это путь к файлу и
    // подпись вложения. Считать их словами заметки нельзя: "![[img/foo-bar.jpg]]"
    // дало бы четыре слова из ничего.
    if (blockImageRef(block).valid) {
        out.image = true;
        return out;
    }
    out.words = countWords(text);
    return out;
}

NoteStats documentStats(const QTextDocument& doc) {
    NoteStats out;
    int lines = 0;
    int breaks = 0;
    for (QTextBlock block = doc.begin(); block.isValid(); block = block.next()) {
        const BlockStats stats = blockStats(block);
        out.words += stats.words;
        out.images += int(stats.image);
        if (stats.breaks > 0) {
            out.marks.push_back({block.blockNumber(), breaks});
            breaks += stats.breaks;
        }
        lines += stats.breaks + 1;
    }
    out.lines = qMax(1, lines);
    out.blocks = doc.blockCount();
    out.valid = true;
    return out;
}

// --- счёт по IR -------------------------------------------------------------

namespace {

// Фотография занимает блок целиком: её путь и подпись словами заметки не
// являются. Правило то же, что у blockImageRef, только заданное об IR: либо
// абзац целиком из image-спанов с одним адресом, либо дословный "![[путь]]"
// (вики-вложение остаётся обычным текстом абзаца — markdown его не трактует).
bool isImageBlock(const Document& ir, const Block& b) {
    if (b.raw || b.kind != Kind::Paragraph) return false;

    const std::span<const Inline> spans = ir.inlines(b);
    if (!spans.empty()) {
        std::string_view href;
        int32_t covered = 0;
        bool all = true;
        for (const Inline& s : spans) {
            if (!s.image() || s.href.empty()) { all = false; break; }
            if (href.empty()) href = ir.href(s);
            else if (href != ir.href(s)) { all = false; break; }
            covered += s.text.size();
        }
        if (all && covered == b.text.size()) return true;
    }

    std::string_view text = ir.text(b);
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.remove_suffix(1);
    return text.size() > 5 && text.starts_with("![[") && text.ends_with("]]");
}

// Во что блок IR превратится в документе: сколько блоков он там займёт и
// сколько мягких переносов останется внутри них.
//
// Литеральное (код и дословные куски) сборщик режет построчно, по блоку на
// строку, и один завершающий перевод снимает; всё прочее живёт одним блоком, а
// переводы внутри становятся мягкими (см. splitLiteralLines и toQt в
// document_builder.cpp). Разделителями там считаются три знака: '\n', '\r' и
// U+2029 — ровно те, которые Qt иначе разорвал бы на блоки.
struct BlockShape {
    int blocks = 1;
    int breaks = 0;
};

BlockShape shapeOf(std::string_view text, bool literal) {
    int newlines = 0;
    int others = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (c == '\n') ++newlines;
        else if (c == '\r') ++others;
        else if (c == 0xE2 && i + 2 < text.size() &&
                 static_cast<unsigned char>(text[i + 1]) == 0x80 &&
                 (static_cast<unsigned char>(text[i + 2]) == 0xA9 ||
                  static_cast<unsigned char>(text[i + 2]) == 0xA8)) {
            // U+2029 сборщик переводит в мягкий перенос сам, а U+2028 уже им и
            // является и доезжает до документа как есть. Заметки из Apple Notes
            // им кишат: без этой ветки счёт по IR разошёлся с документом на трёх
            // заметках корпуса из 274.
            ++others;
            i += 2;
        }
    }
    if (!literal) return {1, newlines + others};
    const bool trailing = !text.empty() && text.back() == '\n';
    return {newlines + 1 - int(trailing), others};
}

}  // namespace

NoteStats irStats(const Document& ir) {
    NoteStats out;
    int block = 0;
    int breaks = 0;
    for (const Block& b : ir.blocks) {
        const std::string_view text = ir.text(b);
        const bool literal = b.raw || b.kind == Kind::Code;
        const BlockShape shape = shapeOf(text, literal);
        if (isImageBlock(ir, b)) ++out.images;
        else out.words += countWords(text);
        if (shape.breaks > 0) {
            // Мягкий перенос в литеральном блоке — редкость ('\r' внутри
            // строки кода): приписываем его первой строке блока, точнее по
            // одному числу не сказать.
            out.marks.push_back({block, breaks});
            breaks += shape.breaks;
        }
        block += shape.blocks;
    }
    out.blocks = qMax(1, block);
    out.lines = qMax(1, block + breaks);
    out.valid = true;
    return out;
}

CaretPlace caretPlace(const NoteStats& stats, const QTextCursor& caret) {
    CaretPlace out;
    if (caret.isNull()) return out;
    const int block = caret.blockNumber();
    const QString text = caret.block().text();
    const QStringView head = QStringView(text).left(caret.positionInBlock());

    // Переносов во всех блоках до текущего: первая метка, стоящая не раньше
    // нашего блока, и говорит, сколько их накопилось.
    int before = 0;
    const auto at = std::lower_bound(
        stats.marks.begin(), stats.marks.end(), block,
        [](const NoteStats::Mark& mark, int value) { return mark.block < value; });
    if (at != stats.marks.end()) before = at->before;
    else before = stats.lines - stats.blocks;   // все переносы, сколько их есть

    out.line = block + before + countLineBreaks(head) + 1;
    const qsizetype cut = head.lastIndexOf(QChar::LineSeparator);
    out.column = int(head.size() - cut);   // cut == -1 даёт size + 1
    return out;
}

}  // namespace zametti
