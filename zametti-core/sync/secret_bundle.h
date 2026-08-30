// SecretBundle — свёрток секретов ОДНОЙ записи связки и его индекс.
//
// Заказ владельца (30.08.2026): в связке устройства лежит одна-единственная
// запись, внутри которой ВСЕ секреты всех хранилищ — не запись-на-секрет.
// Мак первый, но свёрток — общее будущее всех систем: dbus/wincred переедут
// на него следующими заходами.
//
// Здесь ТОЛЬКО формат: разбор, сборка, индекс. Ни связки, ни платформы — их
// держит BundledSecrets (правила одной записи) и платформенные двери под ним.
// Оттого класс гоняется набором на любой системе.
//
// ФОРМАТ — CBOR, как у журнала: {v: 1, stores: {storeId: {слот: байты}}}.
// Слоты ИМЕНОВАННЫЕ ("key", "webdav", "password" — те же слова, что в
// атрибутах dbus и именах wincred): будущий служебный пароль (слова владельца:
// «может ещё какой-то») добавится именем, не ломая формата, а незнакомое имя
// при чтении сохраняется и переживает запись — свёрток один на все версии
// программы. Версию выше своей разбор честно отвергает: переписать такой
// свёрток значило бы молча выкинуть то, чего эта версия не знает.
//
// ИНДЕКС — та же карта БЕЗ секретов: {storeId: [имена слотов]}. Он уезжает в
// атрибут записи и читается без данных — так «есть ли секрет» (has) отвечается
// без системного вопроса связки (довод — secret_store.h, замер 30.08.2026).

#ifndef ZAMETTI_SYNC_SECRET_BUNDLE_H
#define ZAMETTI_SYNC_SECRET_BUNDLE_H

#include "secret_store.h"

#include <QByteArray>
#include <QMap>
#include <QString>

namespace zametti {

class SecretBundle {
public:
    // Разбор байтов записи. Мусор и незнакомая версия — ложь; уже разобранное
    // при этом не трогается (стойкость к повреждённым данным, без Q_ASSERT).
    bool parse(const QByteArray& bytes);

    // Байты для записи в связку. Пустой свёрток — это {v:1, stores:{}}, а не
    // пустые байты: запись связки живёт и пустой (её не пересоздают — ACL).
    QByteArray toBytes() const;

    // Секрет слота; пусто — не хранится.
    QByteArray value(const QString& storeId, SecretStore::Secret which) const;

    // Положить секрет; пустое значение — убрать слот, опустевший storeId
    // выпадает целиком.
    void put(const QString& storeId, SecretStore::Secret which,
             const QByteArray& value);

    // Индекс к текущему содержимому — см. шапку.
    QByteArray index() const;

    // Есть ли слот — по ОДНОМУ ИНДЕКСУ, без свёртка и без секретов.
    // Мусорный индекс — честное «нет».
    static bool indexHas(const QByteArray& index, const QString& storeId,
                         SecretStore::Secret which);

    bool isEmpty() const { return stores_.isEmpty(); }

protected:
    // storeId → (имя слота → байты). QMap, а не QHash: порядок ключей
    // детерминирован, одинаковое содержимое даёт одинаковые байты.
    QMap<QString, QMap<QString, QByteArray>> stores_;
};

}  // namespace zametti

#endif  // ZAMETTI_SYNC_SECRET_BUNDLE_H
