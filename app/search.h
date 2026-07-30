// Поиск по тексту заметок.
//
// Главное правило: поиск видит ровно то, что видит человек. Искомая единица —
// текст блоков IR, то есть содержимое без маркеров разметки; метаданные не
// ищутся вовсе (их в блоках нет — они живут в Document::meta), синтаксис
// эмфазиса не ищется (в тексте блока его уже нет). Совпадение не пересекает
// границу блока: это осознанное ограничение, а не недосмотр — блок и есть
// единица текста.
//
// Виджетов здесь нет намеренно: поиск гоняется в тестах без дисплея и
// исполняется в отдельном потоке, где виджетам делать нечего.

#ifndef ZAMETTI_SEARCH_H
#define ZAMETTI_SEARCH_H

#include "ir.h"

#include <QString>

#include <vector>

namespace zametti {

// Smart case: запрос целиком в нижнем регистре — ищем без учёта регистра;
// есть хоть одна заглавная — с учётом. Сравнение юникодное, через QString:
// наивный ASCII-tolower не знает про кириллицу, и «Дом» не находил бы «дом».
struct Query {
    QString needle;
    bool caseSensitive = false;

    bool isEmpty() const { return needle.isEmpty(); }
    // Шумовой порог: один знак находится в каждой второй заметке, толку от
    // такого списка нет.
    bool tooShort() const { return needle.size() < 2; }
    Qt::CaseSensitivity sensitivity() const {
        return caseSensitive ? Qt::CaseSensitive : Qt::CaseInsensitive;
    }
};

Query makeQuery(const QString& text);

// Текст блока так, как его видит человек: у дословных кусков — сам кусок.
QString blockText(const Block& block);

// Совпадение внутри одной заметки.
struct Hit {
    int block = 0;      // номер блока в Document::blocks
    int offset = 0;     // смещение в тексте блока, в QChar
    int length = 0;
    int ordinal = 0;    // какое это совпадение по счёту в заметке, с нуля
};

std::vector<Hit> findInDocument(const Document& doc, const Query& query);

// Строка с совпадением для списка результатов: сама строка блока, обрезанная
// по краям, и место совпадения внутри неё (для подсветки).
struct HitLine {
    QString text;
    int offset = 0;
    int length = 0;
};

HitLine hitLine(const Document& doc, const Hit& hit, int radius = 48);

}  // namespace zametti

#endif  // ZAMETTI_SEARCH_H
