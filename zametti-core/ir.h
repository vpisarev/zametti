// Промежуточное представление документа.
//
// Транзиентное: живёт только на время загрузки и сохранения. Рядом с
// QTextDocument никогда не существует, синхронизировать его не с чем.
//
// Ядро работает в UTF-8 (std::string). Ни одного include из Qt здесь быть не должно.

#ifndef ZAMETTI_IR_H
#define ZAMETTI_IR_H

#include <string>
#include <vector>

namespace zametti {

enum class Kind {
    Paragraph,
    Heading,          // headingLevel = 1..6
    Code,             // info = язык или пусто
    Quote,
    // Пустая строка — ровно одна. Столько же блоков, сколько пустых строк в
    // файле: пять строк подряд — пять блоков.
    //
    // Отдельным родом, а не полем «сколько пустых строк перед блоком», ради
    // правки: выделение, удаление и перемещение работают с ними как с любыми
    // другими блоками, и «выделили две пустые строки из пяти» не требует
    // особого случая.
    //
    // Имя не Separator: его лучше приберечь для горизонтальной черты '---'.
    VSpace,
    Bullet,           // level = вложенность
    TaskUnchecked,    // level = вложенность
    TaskChecked,      // level = вложенность
    Ordered,          // level = вложенность
};

inline bool isList(Kind k) { return k >= Kind::Bullet; }
inline bool isOrdered(Kind k) { return k == Kind::Ordered; }

struct Span {
    int  offset = 0;    // в байтах, от начала Block::text
    int  length = 0;
    bool bold   = false;
    bool italic = false;
    bool strike = false;
    bool code   = false;   // встроенный код; содержимое буквальное, разметки внутри нет
    std::string href;      // непусто → ссылка
};

struct Block {
    Kind kind         = Kind::Paragraph;
    int  headingLevel = 0;                   // осмысленно только при Kind::Heading
    int  level        = 0;                   // осмысленно только при isList(kind)
    std::string text;                        // чистый текст, без маркеров
    std::string info;                        // осмысленно только при Kind::Code: "cpp", "sh", ...
    std::vector<Span> inlines;
    std::string rawSource;                   // непусто → выводить дословно, остальные поля игнорировать

};

// Слипнутся ли эти два блока, если поставить их в файле подряд без пустой
// строки. Проверено на ядре: абзац после абзаца читается одним абзацем, абзац
// после пункта и после цитаты — их ленивым продолжением. Всё прочее — заголовок,
// список, код, цитата после абзаца — прекрасно стоит вплотную.
//
// Отсюда инвариант IR: между такими блоками обязан стоять VSpace. Тогда
// сериализатору не нужно вставлять пустую строку от себя, и одна пустая строка
// в файле — это ровно один блок VSpace, в обе стороны.
inline bool wouldMerge(const Block& previous, const Block& next) {
    // Два блока кода подряд: их заборы спарились бы не так, как надо, — канон
    // ведь дописывает закрывающий забор незакрытому. Два дословных куска
    // подряд — по той же причине непрозрачности.
    const bool prevLiteral = !previous.rawSource.empty() || previous.kind == Kind::Code;
    const bool nextLiteral = !next.rawSource.empty() || next.kind == Kind::Code;
    if (prevLiteral && nextLiteral) return true;
    if (next.kind != Kind::Paragraph) return false;
    return previous.kind == Kind::Paragraph || previous.kind == Kind::Quote ||
           isList(previous.kind);
}

using Document = std::vector<Block>;

}  // namespace zametti

#endif  // ZAMETTI_IR_H
