// СОЧЕТАНИЯ КЛАВИШ В НАБОРАХ — ТАК ЖЕ, КАК ИХ ЧИТАЕТ БОЙ.
//
// Настройка с сочетанием — это СПИСОК: через точку с запятой их можно
// перечислить несколько («Ctrl+D; Ctrl+SPACE»), и разбирает такой список
// QKeySequence::listFromString, а не QKeySequence(QString) — тот про точку с
// запятой не знает и молча отдаёт мусор (набор при этом остаётся зелёным,
// потому что жмёт несуществующую клавишу и ничего не меняет).
//
// Второе: сочетания принадлежат владельцу и меняются. Набор, вписавший клавишу
// литералом, краснеет на каждой смене вкуса — или, что хуже, тихо перестаёт
// что-либо проверять, когда литерал перестаёт быть привязанным к чему-нибудь.
// Поэтому там, где сочетание НАСТРАИВАЕТСЯ, набор берёт его из настроек, а
// литерал остаётся только там, где клавиша вшита в код.
//
// Разбирает строку тот же key_binding.h, что и бой: набор и окно не могут
// разойтись в том, что считается сочетанием.
#ifndef ZAMETTI_TESTS_KEYS_H
#define ZAMETTI_TESTS_KEYS_H

#include "key_binding.h"

#include <QKeyCombination>
#include <QKeySequence>
#include <QString>
#include <QWidget>
#include <QtTest/QTest>

namespace zt {

// Первое сочетание списка. Пустой список — пустое сочетание: клавиши у команды
// нет вовсе (законный случай — настройка пустой строкой убирает сочетание).
inline QKeySequence firstKey(const QString& setting) {
    const QList<QKeySequence> all = zametti::keySequencesOf(setting);
    return all.isEmpty() ? QKeySequence() : all.first();
}

// Нажать сочетание, записанное как в настройках. false — сочетания нет, жать
// нечего: у вызывающего есть повод сказать об этом вслух, а не сделать вид,
// что нажатие состоялось.
inline bool pressKey(QWidget* target, const QString& setting) {
    const QKeySequence sequence = firstKey(setting);
    if (sequence.isEmpty()) return false;
    const QKeyCombination combo = sequence[0];
    QTest::keyClick(target, combo.key(), combo.keyboardModifiers());
    return true;
}

}  // namespace zt

#endif  // ZAMETTI_TESTS_KEYS_H
