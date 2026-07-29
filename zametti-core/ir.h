// Промежуточное представление документа.
//
// Транзиентное: живёт только на время загрузки и сохранения. Рядом с
// QTextDocument никогда не существует, синхронизировать его не с чем.
//
// Ядро работает в UTF-8 (std::string). Ни одного include из Qt здесь быть не должно.

#ifndef ZAMETTI_IR_H
#define ZAMETTI_IR_H

#include <string>
#include <string_view>
#include <vector>

namespace zametti {

// Род блока — что это за блок, и только. Чем помечен пункт и на каком уровне он
// стоит, родом не выражается: это отдельные оси (Marker, level).
//
// Родов нарочно мало, и ветки default в switch по роду быть не должно. Тогда
// -Wswitch при -Werror сам перечисляет места, где новый род не разобран, — иначе
// код рос бы по квадрату от числа родов, а забытое место молча делало бы не то.
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
    ListItem,         // marker, checked, level
};

// Чем помечен пункт. Выполненность — отдельный признак, а не свой вид маркера:
// переключение задачи это смена bool, а не подмена рода блока.
enum class Marker {
    Bullet,
    Ordered,
    Task,
};

inline bool isList(Kind k) { return k == Kind::ListItem; }

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
    Kind   kind         = Kind::Paragraph;
    Marker marker       = Marker::Bullet;    // осмысленно только при Kind::ListItem
    bool   checked      = false;             // осмысленно только при Marker::Task
    int    headingLevel = 0;                 // осмысленно только при Kind::Heading
    // На каком уровне списка стоит блок. -1 — снаружи списка.
    //
    // Ось общая, а не поле пункта: пункт всегда имеет уровень, но и другие
    // блоки могут стоять внутри пункта — второй абзац, код, цитата. Уровень и
    // говорит, внутри какого пункта они стоят.
    int    level        = -1;
    std::string text;                        // чистый текст, без маркеров
    std::string info;                        // осмысленно только при Kind::Code: "cpp", "sh", ...
    std::vector<Span> inlines;
    std::string rawSource;                   // непусто → выводить дословно, остальные поля игнорировать
};

// Нумерованный ли это пункт и задача ли это. Спрашивать про род тут нечего: род
// у всех пунктов один, различает их маркер.
inline bool isOrdered(const Block& b) {
    return b.kind == Kind::ListItem && b.marker == Marker::Ordered;
}
inline bool isTask(const Block& b) {
    return b.kind == Kind::ListItem && b.marker == Marker::Task;
}

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

// Метаданные заметки — первый блок файла фиксированной формы:
//
//   <!-- zametti
//   parent: 01n6x9k2m4qp
//   created: 2019-03-14T09:26:53Z
//   -->
//
// Хранятся строками между маркером и закрывающей скобкой — дословно и в своём
// порядке: неизвестные ключи обязаны пережить круг побайтово, это forward
// compatibility. Известные ключи читаются и правятся поверх строк.
struct NoteMeta {
    bool present = false;
    // Стояла ли после "-->" пустая строка. В каноне стоит всегда, но флаг
    // нужен: файл без неё не должен меняться от простого открытия.
    bool blankAfter = false;
    std::vector<std::string> lines;   // без перевода строки

    // Значение ключа, обрезанное по краям; пусто — ключа нет.
    std::string get(std::string_view key) const;
    // Правит существующую строку ключа или дописывает новую. Значение не должно
    // содержать "--" (ломает HTML-комментарий) и перевод строки.
    void set(std::string_view key, std::string_view value);
};

struct Document {
    NoteMeta meta;
    std::vector<Block> blocks;
};

}  // namespace zametti

#endif  // ZAMETTI_IR_H
