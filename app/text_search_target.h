// TextSearchTarget — «ИСКОМОЕ»: то, в чём окно ищет по Ctrl+F и ходит по F3.
//
// Долг из отчёта девятой сессии: поиск в окне ветвился пятью `if
// (markdown.active())` — у редактора (NoteView) найденное живёт при заметке и
// адресуется блоками, у вида исходника — смещениями в плоском тексте. С третьим
// видом (редактор конфига) ветвей стало бы восемь. Теперь окно спрашивает у
// активной страницы стека ОДНО: «ты искомое» — и зовёт один и тот же набор
// глаголов. Реализуют NoteView (и через него редактор, вид разности) и
// PlainEditView (вид исходника, вид конфига).
//
// Замена — не у всех: слепок истории только для чтения, и canReplace()
// отвечает за это вместо особых случаев в окне.

#ifndef ZAMETTI_TEXT_SEARCH_TARGET_H
#define ZAMETTI_TEXT_SEARCH_TARGET_H

#include <QString>

class QWidget;

namespace zametti {

class TextSearchTarget {
public:
    virtual ~TextSearchTarget() = default;

    // Найти все вхождения; подсветить; текущее — ближайшее вперёд от каретки.
    // Возвращает число вхождений.
    virtual int findMatches(const QString& text, bool caseSensitive) = 0;
    virtual int matchCount() const = 0;
    // Номер текущего совпадения с нуля; -1 — ни одного.
    virtual int currentMatch() const = 0;
    // Шаг по найденному, циклически.
    virtual void stepMatch(int direction) = 0;
    virtual void clearMatches() = 0;

    virtual bool canReplace() const { return false; }
    // Заменить текущее вхождение / все. Ложь и ноль — нечего или нельзя.
    virtual bool replaceCurrentMatch(const QString& with) { (void)with; return false; }
    virtual int replaceAllMatches(const QString& text, bool caseSensitive, const QString& with) {
        (void)text; (void)caseSensitive; (void)with;
        return 0;
    }

    // Выделенное — готовый запрос поиска (чаще всего ищут то, на что смотрят).
    virtual QString searchPreset() const = 0;
    // Кому вернуть фокус, когда панель поиска закрывается.
    virtual QWidget& searchWidget() = 0;
};

}  // namespace zametti

#endif  // ZAMETTI_TEXT_SEARCH_TARGET_H
