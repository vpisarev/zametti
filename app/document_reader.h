// QTextDocument → IR. Обратный путь к document_builder.
//
// Читатель ничего не угадывает по оформлению: род блока, уровень, язык кода и
// стиль знаков он берёт из свойств (см. doc_model.h), а текст — из самого
// документа. Всё, что оформление добавляет от себя — цвет ссылки, подложка
// кода, увеличенные эмодзи, — на разбор не влияет вовсе.
//
// Проверяемое свойство: read(build(ir)) == ir для любого IR из парсера.

#ifndef ZAMETTI_DOCUMENT_READER_H
#define ZAMETTI_DOCUMENT_READER_H

#include "ir.h"

class QTextDocument;

namespace zametti {

Document readDocument(const QTextDocument& doc);

}  // namespace zametti

#endif  // ZAMETTI_DOCUMENT_READER_H
