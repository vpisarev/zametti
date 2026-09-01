// ZTheme — ЭКРАННЫЕ ЦВЕТА ОДНОЙ СУЩНОСТЬЮ.
//
// Цветов в программе полсотни: страница, каретка, выделение, буллеты, рамки
// чекбоксов, фон плашки кода, значки тулбара, полоса сведений, сниппеты
// списка, подсветка markdown, подсветка JSON. Пока они лежали россыпью в
// config.json, «сделать тёмную тему» означало перебрать полсотни ключей руками
// и не забыть ни одного; забытый ключ выглядел бы как чёрный текст на чёрном.
//
// Теперь у человека РОЛИ (background, foreground, accent, muted…), а раскладку
// ролей по конкретным полям делает одна таблица — applyTo(). Роль называет
// СМЫСЛ («фон панелей»), а не место («фон тулбара и фон полосы сведений»), и
// потому её хватает на всю программу сразу.
//
// Тема бывает встроенная (сегодня одна — light) или своя, файлом
// <configDir>/themes/<имя>.json. Своя может наследовать: "extends": "light"
// берёт встроенную и кладёт поверх свои роли. Цепочка любой длины, цикл —
// ошибка, неизвестное имя — ошибка: молча показать не ту тему, которую
// попросили, хуже, чем сказать вслух.
//
// БУМАГИ ЭТО НЕ КАСАЕТСЯ. У PDF своя типографика и свои цвета (секция pdf):
// экран может быть тёмным, а лист остаётся белым.

#ifndef ZAMETTI_THEME_H
#define ZAMETTI_THEME_H

#include <QColor>
#include <QHash>
#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <vector>

namespace zametti {

struct ZSettings;

class ZTheme {
public:
    // Все известные роли, в порядке объявления: "background", "accent",
    // "markdown.marker", "json.key"… Вложенные роли пишутся через точку, а в
    // файле темы лежат вложенным объектом.
    static QStringList roleNames();
    // То же, но со значением светлой темы и однострочной подписью: из этого
    // собирается раздел темы в шаблоне конфига.
    struct RoleInfo {
        QString name;
        QColor light;
        QString note;
    };
    static std::vector<RoleInfo> roles();
    // Есть ли такая встроенная тема.
    static bool isBuiltin(const QString& name);
    static QStringList builtinNames();

    // Тема по имени: встроенная или своя из <configDir>/themes/<name>.json,
    // вместе со всей цепочкой extends. Ложь — нет такой темы, битый файл или
    // цикл наследования; причина в error, тема при этом не меняется.
    static bool resolve(const QString& name, ZTheme* out, QString* error);

    // Наложить роли из объекта (секция "theme" конфига или файл темы).
    // Ключ "extends" здесь игнорируется — его разбирает resolve. Незнакомые
    // имена ролей попадают в unknown: опечатка в цвете иначе выглядит как
    // «тема не работает».
    void applyOverrides(const QJsonObject& object, QStringList* unknown = nullptr);

    // Цвет роли; неизвестная роль — невалидный QColor.
    QColor color(const QString& role) const;
    bool has(const QString& role) const;
    void setColor(const QString& role, const QColor& value);

    // РАСКЛАДКА РОЛЕЙ ПО ПОЛЯМ — ОДНО МЕСТО НА ВСЮ ПРОГРАММУ. Всё, что красит
    // экран, красится отсюда; кто заводит новый цвет, приходит сюда и называет
    // его роль.
    void applyTo(ZSettings& settings) const;

protected:
    QHash<QString, QColor> colors_;
};

}  // namespace zametti

#endif  // ZAMETTI_THEME_H
