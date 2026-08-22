// ИДЕНТИЧНОСТЬ ХРАНИЛИЩА — видимый `zametti.json` в его корне.
//
// Зачем нужен ОТДЕЛЬНЫЙ файл, а не запись в настройках приложения: идентичность
// обязана путешествовать ВМЕСТЕ С ДАННЫМИ. Скопировали каталог на другую
// машину — копия знает, чьё облако её и где у неё корень; настройки при этом
// остались на прежней машине. По той же причине файл не прячется в `.zametti/`
// (тот каталог не синхронизируется): в облаке его копия служит манифестом, по
// которому сверка идёт ДО ввода пароля.
//
// Что внутри:
//
//   storeId       — 14-значный id того же вида, что у заметок; чеканится один
//                   раз при заведении хранилища;
//   formatVersion — страж совместимости: увидев формат новее своего, программа
//                   вежливо отказывается работать, а не портит данные;
//   created       — когда завели, ISO-8601 с офсетом (для человека);
//   rootNote      — id КОРНЕВОЙ ЗАМЕТКИ, если она уже заведена.
//
// Открытым текстом сознательно: id непрозрачен, утечки в нём нет, а сверка
// «то ли это хранилище» обязана работать раньше расшифровки.

#ifndef ZAMETTI_STORE_IDENTITY_H
#define ZAMETTI_STORE_IDENTITY_H

#include <QString>

namespace zametti::store {

// Версия формата САМОГО ХРАНИЛИЩА (не заметки и не журнала). Поднимается
// только при ломающем изменении раскладки.
inline constexpr int kStoreFormatVersion = 1;

class StoreIdentity {
public:
    static QString pathFor(const QString& root);

    // Прочитать. Файла нет — пустая идентичность (isEmpty), и это не беда:
    // хранилище могло быть заведено прежней сборкой. Файл битый — тоже пустая,
    // но с объяснением в error: молча счесть чужой JSON отсутствующим нельзя.
    static StoreIdentity read(const QString& root, QString* error = nullptr);

    // Отчеканить и записать. Уже есть — читается и возвращается как есть:
    // второй раз id не чеканится ничем и никогда.
    static StoreIdentity ensure(const QString& root, QString* error);

    bool isEmpty() const { return storeId_.isEmpty(); }
    const QString& storeId() const { return storeId_; }
    int formatVersion() const { return formatVersion_; }
    const QString& created() const { return created_; }
    const QString& rootNote() const { return rootNote_; }

    // Формат новее нашего: работать с таким хранилищем нельзя.
    bool tooNew() const { return formatVersion_ > kStoreFormatVersion; }

    // Запомнить адрес корневой заметки и записать файл.
    bool setRootNote(const QString& root, const QString& noteId, QString* error);

protected:
    bool write(const QString& root, QString* error) const;

    QString storeId_;
    int formatVersion_ = kStoreFormatVersion;
    QString created_;
    QString rootNote_;
};

}  // namespace zametti::store

#endif  // ZAMETTI_STORE_IDENTITY_H
