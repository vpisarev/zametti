// Производное от документа заметки — значение с признаком свежести.
//
// У заметки, кроме самого текста, живут вещи, ПОСЧИТАННЫЕ ПО НЕМУ: слова и
// строки, найденное поиском, блоки последней сборки; будут и другие. Все они
// — кэш, и правило у кэша одно (zametti_qtexdocument_design_part2.md):
// источник истины один, кэш всегда можно выбросить и пересчитать, а показывать
// протухшее нельзя — ложь дороже молчания. Чтобы новая кэшируемая величина
// не заводила себе очередной флаг «свеж ли я» россыпью в заметке, она
// заводится ЭТИМ шаблоном:
//
//   Derived<NoteStats> stats_;                       // в ZNote
//   stats_.set(counted, doc().revision());           // посчитали
//   stats_.freshFor(doc().revision())                // показывать ли
//   stats_.invalidate();                             // сказать вслух: устарело
//
// Свежесть — по РЕВИЗИИ документа (QTextDocument::revision растёт на каждую
// правку) плюс явный признак: правка сама делает значение несвежим, а
// invalidate() нужен там, где ревизия не меняется, а смысл — да (подмена
// документа целиком). Величина со своими методами (NoteSearch) держит ту же
// пару «документ + ревизия» сама — правило то же.

#ifndef ZAMETTI_DERIVED_H
#define ZAMETTI_DERIVED_H

#include <utility>

namespace zametti {

template <class T>
class Derived {
public:
    const T& value() const { return value_; }
    // Считалось ли вообще (и не сброшено ли вслух).
    bool valid() const { return valid_; }
    // Свежо для документа этой ревизии: считалось, и с тех пор не правили.
    bool freshFor(int revision) const { return valid_ && revision_ == revision; }
    // При какой ревизии посчитано; -1 — не считалось.
    int revision() const { return revision_; }

    void set(T value, int revision) {
        value_ = std::move(value);
        revision_ = revision;
        valid_ = true;
    }
    void invalidate() { valid_ = false; }

private:
    T value_{};
    int revision_ = -1;
    bool valid_ = false;
};

}  // namespace zametti

#endif  // ZAMETTI_DERIVED_H
