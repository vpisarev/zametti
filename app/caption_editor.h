// Правка подписи картинки прямо под снимком.
//
// Та же механика, что у языка блока кода (lang_editor.h): поле встаёт на место
// подписи, Enter принимает, Esc и уход фокуса отменяют. Пустая подпись
// законна — картинка остаётся картинкой, а в файл уходит «![](путь)».
//
// Спрятанная подпись («~IMG_1234») в поле видна целиком, вместе со знаком:
// человек видит, ПОЧЕМУ её не видно под снимком, и убирает знак, если хочет
// её вернуть.

#ifndef ZAMETTI_CAPTION_EDITOR_H
#define ZAMETTI_CAPTION_EDITOR_H

#include <QColor>
#include <QLineEdit>
#include <QString>

namespace zametti {

class CaptionEditor : public QLineEdit {
    Q_OBJECT

public:
    CaptionEditor(const QString& current, QWidget* parent);

signals:
    void accepted(const QString& caption);
    void cancelled();

protected:
    void keyPressEvent(QKeyEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;

private:
    // Цвет, которым поле закрывает то, что нарисовано под ним.
    QColor backdrop_;
};

}  // namespace zametti

#endif  // ZAMETTI_CAPTION_EDITOR_H
