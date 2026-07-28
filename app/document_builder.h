#ifndef ZAMETTI_DOCUMENT_BUILDER_H
#define ZAMETTI_DOCUMENT_BUILDER_H

#include "ir.h"

#include <QTextBlockFormat>
#include <QtGlobal>

class QTextDocument;

namespace zametti {

// IR → QTextDocument. Единственное место во всём приложении, где UTF-8 ядра
// превращается в UTF-16 Qt.
//
// zoom масштабирует кегль и поля. Отдельного «зума» у QTextEdit не хватает:
// он двигает только шрифт по умолчанию, а у нас кегль задан явно в каждом
// формате — поэтому при смене масштаба документ собирается заново.
void buildDocument(const Document& doc, QTextDocument& target, qreal zoom = 1.0);

// Поле сверху у блока, в высотах строки. Зависит только от самого блока и от
// того, стоит ли перед ним пустая строка.
//
// Отбивки не складываются с авторскими пустыми строками, а лишь заполняют
// пустоту там, где автор не оставил ничего: сколько пустых строк в файле,
// столько и на экране. Живёт отдельной функцией, потому что то же поле
// приходится ставить и операциям, правящим документ на месте, — иначе
// собранный документ и поправленный расходились бы в ритме.
qreal blockTopMargin(Kind kind, bool raw, bool previousIsVSpace, bool first);

// Формат блока пустой строки — ровно такой, каким его собрал бы сборщик. Нужен
// операциям: пустую строку они заводят на живом документе, и отличаться от
// собранной она не имеет права.
QTextBlockFormat vspaceBlockFormat(const QTextDocument& doc, bool previousIsVSpace, bool first);

}  // namespace zametti

#endif  // ZAMETTI_DOCUMENT_BUILDER_H
