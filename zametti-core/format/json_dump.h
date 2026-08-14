#ifndef ZAMETTI_JSON_DUMP_H
#define ZAMETTI_JSON_DUMP_H

#include "ir.h"

#include <string>

namespace zametti {

// IR → JSON. Односторонне, только для golden-тестов и отладки.
// from_json нет и не будет: иначе дамп незаметно станет вторым форматом хранения.
std::string toJson(const Document& doc);

}  // namespace zametti

#endif  // ZAMETTI_JSON_DUMP_H
