#ifndef ZAMETTI_SERIALIZER_H
#define ZAMETTI_SERIALIZER_H

#include "ir.h"

#include <string>

namespace zametti {

// IR → markdown. Вывод всегда канонический: '-' для маркированных списков,
// нумерация с 1, ATX-заголовки, огороженный код.
std::string serialize(const Document& doc);

}  // namespace zametti

#endif  // ZAMETTI_SERIALIZER_H
