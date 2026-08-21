// ОДНОСТРОЧНОЕ ПОЛЕ ВВОДА СО СВОЕЙ КАРЕТКОЙ — основа полей, которые встают
// прямо в тексте: подпись картинки (caption_editor.h) и язык блока кода
// (lang_editor.h).
//
// ПОЧЕМУ НЕ QLineEdit, хотя поле однострочное. У него нельзя погасить штатную
// каретку: `setCursorWidth` есть только у текстовых видов. Пока поля были на
// QLineEdit, своя цветная каретка рисовалась ПОВЕРХ штатной — и владелец
// увидел ровно то же, что когда-то в заметке: «мигают обе, с рассинхроном».
// Приём «перерисовать поверх» в заметке работает потому, что там штатная
// погашена насмерть, а поверх идёт лишь подстраховка от её пробуждения; здесь
// гасить было нечем.
//
// QPlainTextEdit это даёт: `setCursorWidth(0)` — и штатной каретки нет вовсе,
// а своя рисуется цветом caretColor и толщиной caretWidth, мигая общим
// CaretBlink, — как в заметке, в исходнике и в конфиге. Однострочность держим
// сами: переносов нет, полос нет, высота по строке, Enter не переводит строку,
// а принимает ввод.

#ifndef ZAMETTI_LINE_FIELD_H
#define ZAMETTI_LINE_FIELD_H

#include "caret_blink.h"

#include <QColor>
#include <QPlainTextEdit>
#include <QString>

class QPainter;

namespace zametti {

class LineField : public QPlainTextEdit {
    Q_OBJECT

public:
    explicit LineField(QWidget* parent = nullptr);

    // Текст поля одной строкой (переводов строк в нём не бывает).
    QString text() const;
    void setText(const QString& text);
    // Каретка в позицию (знаки от начала); -1 — в конец.
    void setCursorPosition(int position);
    void selectAll();
    // Выключка текста в поле: подпись справа у картинки, прижатой вправо.
    void setAlignment(Qt::Alignment how);
    // Цвет, которым поле закрывает нарисованное под ним. Ставится наследником:
    // под подписью — страница, под именем языка — плашка кода.
    void setBackdrop(const QColor& colour);
    const QColor& backdrop() const { return backdrop_; }
    // Куда встанет следующий знак — в координатах поля. Нужно наследнику,
    // который дорисовывает что-то рядом с кареткой (подсказка языка).
    QRect caretRect() const;

signals:
    void accepted(const QString& text);
    void cancelled();
    // Текст изменился действиями человека (набор, забой, вставка).
    void edited();

protected:
    void keyPressEvent(QKeyEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void focusInEvent(QFocusEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;

private:
    void showCaret();
    // Колонка каретки заново, без штатного курсора (см. .cpp): тот же ход, что
    // у NoteView и PlainEditView.
    void repaintOverNativeCaret(QPainter& painter);

    CaretBlink caret_;
    QColor backdrop_;
};

}  // namespace zametti

#endif  // ZAMETTI_LINE_FIELD_H
