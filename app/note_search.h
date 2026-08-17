// Найденное в показанном документе: запрос и его вхождения.
//
// Раньше это были четыре поля состояния текущей заметки в NoteEditor
// (matches, currentMatch, matchText, matchCaseSensitive) и арифметика над ними
// в пяти методах виджета. Здесь — один объект с методами, который сам держит
// свои инварианты: вхождения идут по возрастанию позиции, текущее либо -1,
// либо номер существующего вхождения, запрос помнится вместе с найденным.
//
// Живёт у ВИДА, а не у заметки: вхождения — курсоры В ПОКАЗАННОМ документе, а
// в режиме истории показан слепок, не живая заметка. Курсоры Qt двигает сам
// при правке; смещения поехали бы от первой же буквы.
//
// Подсветку и переходы делает вид (ему известны окно, каретка и прокрутка);
// здесь — только «что найдено» и ответы на вопросы вида.

#ifndef ZAMETTI_NOTE_SEARCH_H
#define ZAMETTI_NOTE_SEARCH_H

#include <QString>
#include <QTextCursor>

#include <utility>
#include <vector>

class QTextDocument;

namespace zametti {

class NoteSearch {
public:
    // Найти все вхождения text в doc, включая перекрывающиеся (счётчик обязан
    // считать их так же, как их обойдёт F3). Пустой запрос — пусто. Текущее
    // сбрасывается. Возвращает число найденного.
    int find(const QTextDocument& doc, const QString& text, bool caseSensitive);
    void clear();

    bool empty() const { return hits_.empty(); }
    int count() const { return int(hits_.size()); }
    const QString& text() const { return text_; }
    bool caseSensitive() const { return caseSensitive_; }
    const QTextCursor& hit(int index) const { return hits_[size_t(index)]; }

    // Текущее вхождение: -1 — не выбрано.
    int current() const { return current_; }
    bool hasCurrent() const { return current_ >= 0 && current_ < count(); }
    // Выбрать по номеру — по кругу: -1 это последнее, count — первое.
    // Пусто — остаётся -1.
    void setCurrent(int index);
    // Ближайшее вхождение от позиции: вперёд — первое, начинающееся не раньше
    // position; назад — последнее, кончающееся не позже. -1 — нет.
    int nearestForward(int position) const;
    int nearestBackward(int position) const;

    // Вхождения, задевающие окно позиций [from, to]: полуинтервал номеров
    // [first, last). Двоичным поиском — вхождения упорядочены.
    std::pair<int, int> range(int from, int to) const;

protected:
    std::vector<QTextCursor> hits_;
    int current_ = -1;
    QString text_;
    bool caseSensitive_ = false;
};

}  // namespace zametti

#endif  // ZAMETTI_NOTE_SEARCH_H
