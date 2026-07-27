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

    // Блок стоит вплотную к предыдущему: пустой строки между ними в файле нет.
    //
    // Пустая строка в markdown — не украшение: между двумя блоками она бывает
    // обязательной (два абзаца), а бывает и содержательной (список сразу под
    // вводной фразой против списка через строку). Без этого признака показать
    // разницу нечем: редактор рисовал одинаковый зазор и там, и там, и пустые
    // строки из файла попросту не были видны.
    //
    // У первого блока смысла не имеет.
    bool tight = false;
};

using Document = std::vector<Block>;

}  // namespace zametti

#endif  // ZAMETTI_IR_H
