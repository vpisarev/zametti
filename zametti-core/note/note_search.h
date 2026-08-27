// Найденное в показанном документе: запрос и его вхождения.
//
// Раньше это были четыре поля состояния текущей заметки в NoteEditor
// (matches, currentMatch, matchText, matchCaseSensitive) и арифметика над ними
// в пяти методах виджета. Здесь — один объект с методами, который сам держит
// свои инварианты: вхождения идут по возрастанию позиции, текущее либо -1,
// либо номер существующего вхождения, запрос помнится вместе с найденным.
//
// Живёт В ЗАМЕТКЕ (ZNote::search) — это КЭШ ПОИСКА (решение владельца):
// вернулись к заметке — найденное и номер текущего при ней; заметка перечитана
// с диска (отпечаток разошёлся) — объект заметки новый, и кэш обнулён вместе с
// ним, а не заменён на «нет совпадений». Свежесть найденного стережёт ревизия
// документа: правка меняет её, и поиск с тем же запросом идёт заново. В
// режиме истории найденное в документе разности живёт при таймлайне
// (ZNoteTimeline::search) — тем же классом.
//
// Подсветку и переходы делает вид (ему известны окно, каретка и прокрутка);
// здесь — только «что найдено» и ответы на вопросы вида.

#ifndef ZAMETTI_NOTE_SEARCH_H
#define ZAMETTI_NOTE_SEARCH_H

#include "search.h"

#include <QString>
#include <QTextCursor>

#include <utility>
#include <vector>

class QTextDocument;

namespace zametti {

// Одно вхождение: курсор над найденным (едет с правками, как всякий курсор
// Qt) и, если вхождение ВНУТРИ ОБЪЕКТА (таблица, формула — в тексте блока один
// U+FFFC, а искали по исходнику), смещение и длина внутри исходника. Курсор у
// такого вхождения стоит над самим знаком объекта.
struct SearchHit {
    QTextCursor cursor;
    int innerOffset = -1;   // < 0 — обычное вхождение в тексте
    int innerLength = 0;
    bool inObject() const { return innerOffset >= 0; }
};

class NoteSearch {
public:
    // Найти все вхождения запроса в doc. Пустой запрос — пусто. Текущее
    // сбрасывается. Возвращает число найденного. Ищется ОБЩИМ перечислителем
    // (forEachHit): объекты — по исходнику, порядок вхождений тот же, что у
    // ZDocument::find (по нему ходит список результатов), перекрывающихся
    // вхождений нет, пустые не считаются.
    int find(const QTextDocument& doc, const Query& query);
    void clear();
    // Найденное свежо для этого документа и запроса: те же буквы, тот же
    // регистр, тот же признак выражения, документ с тех пор не правили.
    bool isFreshFor(const QTextDocument& doc, const Query& query) const;

    bool empty() const { return hits_.empty(); }
    int count() const { return int(hits_.size()); }
    // Запрос, которым нашли: его же повторяют после правки.
    const Query& query() const { return query_; }
    const QString& text() const { return query_.needle; }
    const QTextCursor& hit(int index) const { return hits_[size_t(index)].cursor; }
    const SearchHit& hitAt(int index) const { return hits_[size_t(index)]; }

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
    // Вхождение, которое РОВНО совпадает с выделением [from, to): -1 — нет.
    // Так, вернувшись к заметке, где каретка стоит на находке, счётчик снова
    // показывает её номер, а не «ни одного».
    int indexOfSelection(int from, int to) const;

    // Вхождения, задевающие окно позиций [from, to]: полуинтервал номеров
    // [first, last). Двоичным поиском — вхождения упорядочены.
    std::pair<int, int> range(int from, int to) const;

protected:
    std::vector<SearchHit> hits_;
    int current_ = -1;
    Query query_;
    const QTextDocument* doc_ = nullptr;   // в каком документе искали
    int revision_ = -1;                     // и какой он был ревизии
};

}  // namespace zametti

#endif  // ZAMETTI_NOTE_SEARCH_H
