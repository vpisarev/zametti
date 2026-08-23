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

#include <QByteArray>
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

    // ВЕРСИЯ ФОРМАТА ЗАМЕТКИ (refactor3, решение владельца): `version: 1`
    // ставится ЛЕНИВО — только заметкам, которые мы пишем: свежим, ввезённым и
    // тем, что правим (штамп modified). Открыть и посмотреть — файл не трогает.
    // Нет ключа — версия 1 по умолчанию; более новую версию не понижаем и не
    // трогаем. Встаёт первой строкой шапки: это объявление формата, а не
    // свойство заметки.
    void ensureVersion();
    static constexpr const char* kVersionKey = "version";
    static constexpr const char* kFormatVersion = "1";

    // ОДНО ЛИ ЭТО СОДЕРЖИМОЕ, если не считать штампов, которые ставит сама
    // программа. Байты файлов целиком, а не шапки: спрашивают об этом там, где
    // на руках две записанные версии заметки (журнал, автосохранение).
    //
    // Штампов два. `modified` меняется на каждой записи и сам изменением
    // заметки не является: без этой оговорки «изменилось ли» отвечало бы «да»
    // всегда, и каждая пауза в наборе давала бы на диске новую копию, а в
    // истории — запись, отличающуюся одной цифрой в дате. `version` встаёт в
    // шапку ЛЕНИВО, при первой записи правленой заметки, — и без оговорки
    // возврат отменой к состоянию, записанному до неё, считался бы новой
    // записью журнала (поймал набор HistoryWrite).
    //
    // Метод шапки, а не журнала: журнал о формате заметки не знает вовсе, у
    // него безымянные байты.
    static bool sameFileApartFromStamps(const QByteArray& a, const QByteArray& b);

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
