#include "search.h"

namespace zametti {

Query makeQuery(const QString& text) {
    Query query;
    query.needle = text;
    // Регистр важен, если человек сам его задал. Сравниваем с нижним
    // регистром, а не ищем заглавные по алфавиту: у кириллицы и у любого
    // другого письма свои правила, и знать их — дело QString.
    query.caseSensitive = text != text.toLower();
    return query;
}

}  // namespace zametti
