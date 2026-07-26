// Буллет маркированного списка, нарисованный приложением.
//
// Знаком из шрифта его размер не задать независимо от положения: "•"
// центрируется по высоте строчных своего кегля, и стоит увеличить его — как он
// уезжает вверх относительно текста. Кружок рисуется от метрик самой строки,
// поэтому растёт на месте, а поправку по вертикали можно задать отдельно.

#ifndef ZAMETTI_BULLET_OBJECT_H
#define ZAMETTI_BULLET_OBJECT_H

#include <QObject>
#include <QTextFormat>
#include <QTextObjectInterface>

namespace zametti {

class BulletObject : public QObject, public QTextObjectInterface {
    Q_OBJECT
    Q_INTERFACES(QTextObjectInterface)

public:
    enum { Type = QTextFormat::UserObject + 2 };

    using QObject::QObject;

    QSizeF intrinsicSize(QTextDocument* doc, int posInDocument,
                         const QTextFormat& format) override;
    void drawObject(QPainter* painter, const QRectF& rect, QTextDocument* doc,
                    int posInDocument, const QTextFormat& format) override;
};

}  // namespace zametti

#endif  // ZAMETTI_BULLET_OBJECT_H
