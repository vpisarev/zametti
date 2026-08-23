// ВИДИМАЯ ИДЕНТИЧНОСТЬ ХРАНИЛИЩА — файл `zametti.json` рядом с заметками.
//
//     { "storeId": "01n6cqevh7bbfr", "formatVersion": 1,
//       "created": "2026-08-23T00:00:00+03:00", "rootNote": "01n6cqevsd7v5e" }
//
// ПОЧЕМУ РЯДОМ С ДАННЫМИ, А НЕ В НАСТРОЙКАХ. Копия каталога обязана знать,
// чьё она облако: перенёс человек хранилище на другую машину — и оно узнаётся
// само, без настройки. В облаке этот же файл служит МАНИФЕСТОМ, по которому
// сверка идёт ДО ввода пароля: «то ли это хранилище» решается раньше, чем
// расшифровывается хоть один блоб.
//
// ЧИСТОЕ ЗНАЧЕНИЕ, БЕЗ ФАЙЛОВ. Разбор и сборка живут здесь, а чтение и запись —
// у хранилища (ZStorage::identity, ensureIdentity, setRootNote): файлы трогает
// только оно (решение владельца, refactor4). Поэтому набор проверяет разбор
// без единого файла на диске.
//
// СТРАЖ ВЕРСИИ. Формат новее нашего — не работаем, а не портим: сборка, не
// знающая половины ключей, перепишет файл без них, и старшая версия потеряет
// данные молча. Незнакомые ключи ВНУТРИ своей версии, наоборот, пропускаются и
// сохраняются при перезаписи (обещание с этапа 7).

#ifndef ZAMETTI_STORE_IDENTITY_H
#define ZAMETTI_STORE_IDENTITY_H

#include <QByteArray>
#include <QJsonObject>
#include <QString>

namespace zametti::store {

// Версия формата ХРАНИЛИЩА (не заметки и не журнала): её поднимает только
// ломающее изменение раскладки каталога.
inline constexpr int kStoreFormatVersion = 1;

// Имя файла — здесь один раз.
inline constexpr char kIdentityFile[] = "zametti.json";

class StoreIdentity {
public:
    StoreIdentity() = default;

    // Разбор байтов файла. false — не JSON, не объект или id не годится;
    // объяснение в error. Отсутствие файла разбирать нечем: это не ошибка
    // разбора, а вопрос к хранилищу.
    bool parse(const QByteArray& bytes, QString* error = nullptr);

    // Байты для записи. Незнакомые ключи, прочитанные разбором, сохраняются:
    // старшая сборка добавила своё — не нам это стирать.
    QByteArray toBytes() const;

    // Свежая идентичность: id чеканится один раз и не меняется никогда.
    static StoreIdentity mint(const QString& storeId, const QString& createdIso);

    bool isEmpty() const { return storeId_.isEmpty(); }
    const QString& storeId() const { return storeId_; }
    int formatVersion() const { return formatVersion_; }
    const QString& created() const { return created_; }
    const QString& rootNote() const { return rootNote_; }

    // Формат новее нашего: работать нельзя.
    bool tooNew() const { return formatVersion_ > kStoreFormatVersion; }

    // Адрес корневой заметки. Единственное, что в этом файле меняется.
    void setRootNote(const QString& id) { rootNote_ = id; }

protected:
    QString storeId_;
    int formatVersion_ = kStoreFormatVersion;
    QString created_;
    QString rootNote_;
    // Всё, чего мы не знаем: едет обратно в файл при перезаписи.
    QJsonObject extra_;
};

}  // namespace zametti::store

#endif  // ZAMETTI_STORE_IDENTITY_H
