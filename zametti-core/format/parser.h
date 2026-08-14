#ifndef ZAMETTI_PARSER_H
#define ZAMETTI_PARSER_H

#include "ir.h"

// Умирающие подписи сборки от представления приезжают вместе с этими:
// потребители у них одни и те же, и менять их до сноса IR незачем.
#include "../doc/ir_bridge.h"

#include <cstddef>
#include <string_view>

namespace zametti {

// Сколько байт арены резервируется под разбор источника этой длины. В арену
// уезжают текст блоков, дословные куски, info-строки и адреса — всё это куски
// исходника, и суммарно они его не превосходят (замер на корпусах: k ≈ 0.9).
// Запас нужен на переезды при правке текста внутри самого разбора.
//
// Значение открыто наружу ради проверки инварианта «ноль реаллокаций арены»:
// иначе тест не с чем сравнивать.
constexpr size_t arenaReserveFor(size_t sourceLength) {
    return sourceLength + sourceLength / 8 + 1024;
}

// markdown → IR.
//
// Принимает всё, что принимает CommonMark + расширения GitHub (таблицы,
// зачёркивание, чекбоксы). Конструкции, непредставимые в IR, сохраняются
// дословно в Block::rawSource — потерять молча ничего нельзя.
Document parse(std::string_view markdown);

}  // namespace zametti

#endif  // ZAMETTI_PARSER_H
