#ifndef ZAMETTI_PARSER_H
#define ZAMETTI_PARSER_H

#include "ir.h"

#include <string_view>

namespace zametti {

// markdown → IR.
//
// Принимает всё, что принимает CommonMark + расширения GitHub (таблицы,
// зачёркивание, чекбоксы). Конструкции, непредставимые в IR, сохраняются
// дословно в Block::rawSource — потерять молча ничего нельзя.
Document parse(std::string_view markdown);

}  // namespace zametti

#endif  // ZAMETTI_PARSER_H
