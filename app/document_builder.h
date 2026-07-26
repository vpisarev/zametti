#ifndef ZAMETTI_DOCUMENT_BUILDER_H
#define ZAMETTI_DOCUMENT_BUILDER_H

#include "ir.h"

#include <QtGlobal>

class QTextDocument;
class QWidget;

namespace zametti {

// IR → QTextDocument. Единственное место во всём приложении, где UTF-8 ядра
// превращается в UTF-16 Qt.
//
// zoom масштабирует кегль и поля. Отдельного «зума» у QTextEdit не хватает:
// он двигает только шрифт по умолчанию, а у нас кегль задан явно в каждом
// формате — поэтому при смене масштаба документ собирается заново.
void buildDocument(const Document& doc, QTextDocument& target, qreal zoom = 1.0);

// Цвета страницы и выделения. Живут рядом с остальным оформлением, чтобы вся
// палитра задавалась в одном файле.
void applyPalette(QWidget& view);

}  // namespace zametti

#endif  // ZAMETTI_DOCUMENT_BUILDER_H
