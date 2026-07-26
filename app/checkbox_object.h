#ifndef ZAMETTI_CHECKBOX_OBJECT_H
#define ZAMETTI_CHECKBOX_OBJECT_H

#include <QFont>
#include <QObject>
#include <QTextFormat>
#include <QTextObjectInterface>

namespace zametti {

// Чекбокс, нарисованный самим приложением, а не взятый из шрифта.
//
// Шрифтовые варианты (☐ из DejaVu, "[ ]" из основной гарнитуры) упираются в то,
// что размер, толщина линий и положение по базовой линии заданы шрифтом и не
// настраиваются. Свой объект даёт и то, и другое, и третье, а заодно — точный
// прямоугольник, по которому потом можно будет ловить клик.
class CheckboxObject : public QObject, public QTextObjectInterface {
    Q_OBJECT
    Q_INTERFACES(QTextObjectInterface)

public:
    enum { Type = QTextFormat::UserObject + 1 };
    enum { CheckedProperty = QTextFormat::UserProperty + 1 };

    // Сторона рамки. Нужна и снаружи: от неё считается колонка текста задачи.
    static qreal sideFor(const QFont& font);

    using QObject::QObject;

    QSizeF intrinsicSize(QTextDocument* doc, int posInDocument,
                         const QTextFormat& format) override;
    void drawObject(QPainter* painter, const QRectF& rect, QTextDocument* doc,
                    int posInDocument, const QTextFormat& format) override;
};

}  // namespace zametti

#endif  // ZAMETTI_CHECKBOX_OBJECT_H
