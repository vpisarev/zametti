#ifndef ZAMETTI_DOCUMENT_BUILDER_H
#define ZAMETTI_DOCUMENT_BUILDER_H

#include "ir.h"

class QTextDocument;

namespace zametti {

// IR → QTextDocument. Единственное место во всём приложении, где UTF-8 ядра
// превращается в UTF-16 Qt.
void buildDocument(const Document& doc, QTextDocument& target);

}  // namespace zametti

#endif  // ZAMETTI_DOCUMENT_BUILDER_H
