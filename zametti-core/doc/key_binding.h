// СОЧЕТАНИЯ КЛАВИШ ИЗ НАСТРОЕК — один разборщик на всех.
//
// Настройка с сочетанием — это СПИСОК через точку с запятой («Ctrl+D;
// Ctrl+SPACE»): разбирает его QKeySequence::listFromString, а не
// QKeySequence(QString) — тот про точку с запятой не знает и молча отдаёт
// Key_unknown при count() == 1 (инцидент сессии 8: набор жал пустоту и молчал).
// Пустая строка — сочетания нет вовсе (законный способ его убрать).
//
// Сопоставлять нажатие с сочетанием обязана Qt (QKeySequence::matches), а не
// разбор события руками: с зажатым Shift event->key() приходит знаком верхнего
// регистра, и сравнение с Qt::Key_2 не срабатывает никогда.
//
// Здесь, в ядре (QtGui), а не в каждом виджете: обычный вид, вид исходника,
// окно и наборы спрашивают ОДНО правило — иначе «одинаково ли воспринимаются
// сочетания в двух режимах» держалось бы на дисциплине, а не на устройстве.

#ifndef ZAMETTI_KEY_BINDING_H
#define ZAMETTI_KEY_BINDING_H

#include <QKeyEvent>
#include <QKeySequence>
#include <QList>
#include <QString>

namespace zametti {

// Все сочетания настройки, без пустых. Пустой список — клавиши у команды нет.
inline QList<QKeySequence> keySequencesOf(const QString& setting) {
    QList<QKeySequence> out;
    for (const QKeySequence& keys :
         QKeySequence::listFromString(setting, QKeySequence::PortableText))
        if (!keys.isEmpty()) out.push_back(keys);
    return out;
}

// Это нажатие — ровно это сочетание (с учётом раскладки, как сопоставляет Qt).
inline bool keyEventMatches(const QKeyEvent& event, const QKeySequence& keys) {
    return !keys.isEmpty() &&
           QKeySequence(event.keyCombination()).matches(keys) == QKeySequence::ExactMatch;
}

// Это нажатие — одно из сочетаний списка.
inline bool keyEventMatchesAny(const QKeyEvent& event, const QList<QKeySequence>& list) {
    for (const QKeySequence& keys : list)
        if (keyEventMatches(event, keys)) return true;
    return false;
}

// Это нажатие — одно из сочетаний настройки. Разбор строки на каждое нажатие —
// для редких проверок; горячий путь держит разобранный список у себя.
inline bool keyEventMatches(const QKeyEvent& event, const QString& setting) {
    return keyEventMatchesAny(event, keySequencesOf(setting));
}

}  // namespace zametti

#endif  // ZAMETTI_KEY_BINDING_H
