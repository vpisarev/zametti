// ВРЕМЕННЫЕ ПОДПИСИ УМИРАЮЩЕГО ПРЕДСТАВЛЕНИЯ.
//
// Промежуточное представление (zametti::Document — арена блоков и спанов)
// сносится. Пока его последние потребители не переведены на ZDocument, старые
// подписи остаются — но ведут они уже в новый разбор и в новую сборку.
//
// Смысл мостика не в удобстве, а в СЕТИ: через parse(), serialize() и
// buildDocument() сегодня проходят все корпусные проверки — круг на заметках
// владельца, идемпотентность на спецификациях CommonMark и GFM, фаззинг
// операций. Направив их в новый код, я получаю на нём всё прежнее покрытие
// сразу, а не после того, как перепишу каждого потребителя.
//
// Заголовок включается из parser.h и serializer.h, чтобы старым потребителям
// не пришлось менять ни строки. Умрёт вместе с ними.

#ifndef ZAMETTI_IR_BRIDGE_H
#define ZAMETTI_IR_BRIDGE_H

#include "document_builder.h"

#include <vector>

namespace zametti {

struct Document;

// Представление → логические блоки. Шапка в блоки не входит: у представления
// она своя (Document::meta).
std::vector<Piece> piecesOf(const Document& ir);

// Сборка и заплатка, но от представления. Ровно piecesOf + вызов.
void buildDocument(const Document& ir, QTextDocument& target, qreal zoom = 1.0);
bool patchDocument(const Document& built, const Document& now, const Document& to,
                   QTextDocument& target, qreal zoom = 1.0);

}  // namespace zametti

#endif  // ZAMETTI_IR_BRIDGE_H
