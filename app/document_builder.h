#ifndef ZAMETTI_DOCUMENT_BUILDER_H
#define ZAMETTI_DOCUMENT_BUILDER_H

#include "ir.h"

class QTextDocument;
class QWidget;

namespace zametti {

// IR → QTextDocument. Единственное место во всём приложении, где UTF-8 ядра
// превращается в UTF-16 Qt.
void buildDocument(const Document& doc, QTextDocument& target);

// Цвета страницы и выделения. Живут рядом с остальным оформлением, чтобы вся
// палитра задавалась в одном файле.
void applyPalette(QWidget& view);

}  // namespace zametti

#endif  // ZAMETTI_DOCUMENT_BUILDER_H
