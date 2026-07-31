// История правок — над IR, а не над QTextDocument.
//
// Встроенный стек QTextDocument для нашего правила не годится по двум причинам
// сразу. Он хранит и смену форматов — то есть новый шрифт или цвет попали бы в
// историю наравне с набранным текстом. И он умирает при пересборке документа, а
// пересборка и есть способ применить новый облик.
//
// Правило: документ — это содержимое, а не внешний облик, и undo на облик не
// распространяется. Набрали «мама мыла раму», нажали Ctrl+=, потом undo —
// получится «мама мыла» увеличенным шрифтом. Смена оформления в историю не
// попадает вовсе: документ просто собирается заново из текущего содержимого.
//
// Интерфейс намеренно узкий — снимок туда, снимок обратно. Если снимки окажутся
// дороги (замер: копия IR заметки в 141 КБ — 50 мкс), внутренность можно
// заменить на разности или общий буфер, не трогая ни операции, ни виджет.
//
// Глубина ограничена с двух сторон: числом шагов и суммарным весом. Одного
// счёта шагов мало — замер на заметке в 239 КБ дал 361 КБ на шаг, то есть
// 72 МБ на 200 шагов за одну заметку. Разности это сняли бы совсем; бюджет
// дешевле и потолок задаёт уже сейчас.

#ifndef ZAMETTI_EDIT_HISTORY_H
#define ZAMETTI_EDIT_HISTORY_H

#include "ir.h"

#include <deque>

namespace zametti {

// Шаг истории: содержимое и место курсора в нём. Место — смещение в тексте
// документа; маркеры списка в текст не входят, поэтому оно не зависит ни от
// оформления, ни от нумерации.
struct HistoryStep {
    Document doc;
    int cursor = 0;
};

class EditHistory {
public:
    explicit EditHistory(int limit = 200, size_t budgetBytes = 32u * 1024 * 1024)
        : limit_(limit > 1 ? limit : 2), budget_(budgetBytes) {}

    // Начало работы с файлом: история обнуляется, откатывать нечего.
    void reset(Document doc, int cursor);

    // Новый шаг. Всё, что было впереди (отменённое и не переделанное),
    // отбрасывается — как во всех редакторах.
    void push(Document doc, int cursor);

    // Дописать в текущий шаг вместо нового. Так набор подряд идущих букв
    // остаётся одним шагом: иначе Ctrl+Z возвращал бы по одной букве.
    void amend(Document doc, int cursor);

    bool canUndo() const { return position_ > 0; }
    bool canRedo() const { return position_ + 1 < steps_.size(); }

    // Возвращают nullptr, если идти некуда.
    const HistoryStep* undo();
    const HistoryStep* redo();

    const HistoryStep& current() const { return steps_[position_]; }
    size_t size() const { return steps_.size(); }
    // Сколько памяти занимают снимки: арена, спаны и блоки каждого шага.
    size_t bytes() const;

private:
    void dropOldestIfNeeded();

    std::deque<HistoryStep> steps_{HistoryStep{}};
    size_t position_ = 0;
    int limit_;
    size_t budget_;
};

}  // namespace zametti

#endif  // ZAMETTI_EDIT_HISTORY_H
