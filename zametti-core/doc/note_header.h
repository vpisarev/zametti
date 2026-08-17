// Шапка заметки: `<!-- zametti ... -->` в начале файла.
//
// Строки хранятся ДОСЛОВНО, и это несущее правило хранилища: круг
// чтение-запись побайтовый, включая порядок строк и НЕЗНАКОМЫЕ КЛЮЧИ. Свой
// ключ правится заменой своей строки на каноническую «key: value»; чужие строки
// не трогаются никогда.
//
// Жило это в ir.h вместе с промежуточным представлением, хотя к нему отношения
// не имеет вовсе: шапка — не разметка, а свойства заметки.

#pragma once

#include <QString>

#include <string>
#include <string_view>
#include <vector>

namespace zametti {

class NoteHeader {
public:
    // Значение ключа; пусто — ключа нет.
    std::string get(std::string_view key) const;
    // Пустое значение СНИМАЕТ ключ: отсутствующий parent и значит «в корне».
    void set(std::string_view key, std::string_view value);
    void unset(std::string_view key);

    bool present() const { return present_; }
    void setPresent(bool present) { present_ = present; }

    // Была ли пустая строка между шапкой и телом. Часть побайтового круга.
    bool blankAfter() const { return blankAfter_; }
    void setBlankAfter(bool blank) { blankAfter_ = blank; }

    const std::vector<std::string>& lines() const { return lines_; }
    void setLines(std::vector<std::string> lines) { lines_ = std::move(lines); }

    // Байты шапки, как они уйдут в файл. Пусто — шапки нет.
    std::string toBytes() const;
    // То же текстом (QString): писатель собирает файл текстом и переводит в
    // байты один раз на границе.
    QString toText() const;

    friend bool operator==(const NoteHeader& a, const NoteHeader& b) {
        return a.present_ == b.present_ && a.blankAfter_ == b.blankAfter_ &&
               a.lines_ == b.lines_;
    }

protected:
    std::vector<std::string> lines_;
    bool present_ = false;
    bool blankAfter_ = false;
};

}  // namespace zametti
