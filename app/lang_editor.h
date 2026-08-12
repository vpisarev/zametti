// Ввод языка блока кода прямо в его полоске.
//
// Та же механика, что у переименования папки в дереве: поле встаёт на место
// надписи, Enter принимает, Esc отменяет, пустое имя убирает язык.
//
// Дополнение берётся ИЗ ТЕКУЩЕЙ ЗАМЕТКИ и ниоткуда больше (решение владельца:
// «текущая заметка = текущая колонка Excel»). Отдельной памяти языков у
// программы нет — и не надо: список языков, которыми человек пишет вот эту
// заметку, и есть самый точный список.
//
// Дописанный хвост рисуется серым, а не выделяется, как это делает QCompleter:
// выделение читается как «это уже введено», а хвост введён не человеком.

#ifndef ZAMETTI_LANG_EDITOR_H
#define ZAMETTI_LANG_EDITOR_H

#include <QLineEdit>
#include <QString>
#include <QStringList>

namespace zametti {

class LanguageEditor : public QLineEdit {
    Q_OBJECT

public:
    LanguageEditor(const QStringList& candidates, const QString& current, QWidget* parent);

    // Введённое вместе с принятым дополнением.
    QString language() const;
    // Хвост, дописанный серым; пусто — дополнять нечем.
    QString completion() const { return completion_; }

signals:
    void accepted(const QString& language);
    void cancelled();

protected:
    void keyPressEvent(QKeyEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;

private:
    void updateCompletion();
    // Принять дополнение в сам текст (Tab и стрелка вправо у конца строки).
    bool takeCompletion();

    QStringList candidates_;
    QString completion_;
};

}  // namespace zametti

#endif  // ZAMETTI_LANG_EDITOR_H
