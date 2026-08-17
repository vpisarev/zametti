// ZStorage — хранилище как объект: каталог по id, путь и журнал по id, правка
// шапки закрытой заметки штатным путём записи.
//
// Тесты как критерий дизайна: каталог совпадает с диском после reload и после
// refreshNote; исчезнувший файл уходит из каталога; rewriteNote пишет так же,
// как редактор (самопроверка, атомарно, шаг журнала) и обновляет каталог; вне
// хранилища всё пусто и ничего не падает.

#include "zstorage.h"
#include "store.h"
#include "test_util.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <string>

namespace {

using zametti::ZStorage;

std::string s(const QString& q) { return q.toStdString(); }
std::string n(long long v) { return std::to_string(v); }

zametti::history::Rules rules() { return zametti::history::Rules{}; }

void checkCatalog() {
    QTemporaryDir home;
    const QString root = home.path() + QStringLiteral("/store");
    QString error;
    ZT_TRUE("хранилище заведено: " + s(error), zametti::store::initStore(root, &error));
    const QString folderPath = zametti::store::newNote(root, QString(), &error);
    ZT_TRUE("папка создана: " + s(error), !folderPath.isEmpty());
    const QString folderId = ZStorage::idOfPath(folderPath);
    {
        // Папка — заметка с role: folder и заголовком.
        QFile f(folderPath);
        ZT_TRUE("папка открыта", f.open(QIODevice::ReadWrite));
        QString text = QString::fromUtf8(f.readAll());
        text.replace(QStringLiteral("-->"), QStringLiteral("role: folder\nsort: name-asc\n-->"));
        text += QStringLiteral("# Проекты\n");
        f.resize(0);
        f.write(text.toUtf8());
    }
    const QString notePath = zametti::store::newNote(root, folderId, &error);
    ZT_TRUE("заметка в папке создана: " + s(error), !notePath.isEmpty());
    const QString noteId = ZStorage::idOfPath(notePath);
    {
        QFile f(notePath);
        ZT_TRUE("заметка открыта", f.open(QIODevice::Append));
        f.write("# Первая\n\nтекст заметки\n");
    }

    ZStorage storage(root + QStringLiteral("/"));   // хвостовой слэш чистится у двери
    ZT_TRUE("это хранилище", storage.isStore());
    ZT_EQ("корень чистый", s(QDir::cleanPath(root)), s(storage.root()));
    storage.reload();
    ZT_EQ("две записи в каталоге", n(2), n(storage.count()));
    const ZStorage::NoteInfo* folder = storage.info(folderId);
    const ZStorage::NoteInfo* note = storage.info(noteId);
    ZT_TRUE("папка в каталоге", folder != nullptr && folder->folder());
    ZT_TRUE("метка сортировки прочитана", folder != nullptr && folder->sortMark().has_value());
    ZT_TRUE("заметка в каталоге с родителем", note != nullptr && note->parent() == folderId);
    ZT_EQ("заголовок заметки", "Первая", s(note != nullptr ? note->title() : QString()));
    ZT_EQ("путь по id", s(QDir::cleanPath(notePath)), s(storage.pathOf(noteId)));
    ZT_TRUE("журнал заметки доступен", storage.historyOf(noteId, rules()).available());

    // Файл поменяли снаружи — refreshNote видит новый заголовок.
    {
        QFile f(notePath);
        ZT_TRUE("заметка открыта на дозапись", f.open(QIODevice::ReadWrite));
        QString text = QString::fromUtf8(f.readAll());
        text.replace(QStringLiteral("# Первая"), QStringLiteral("# Вторая"));
        f.resize(0);
        f.write(text.toUtf8());
    }
    ZT_TRUE("перечитана", storage.refreshNote(noteId));
    ZT_EQ("новый заголовок в каталоге", "Вторая", s(storage.info(noteId)->title()));

    // Правка шапки закрытой заметки — штатным путём: заголовок и родитель
    // меняются, файл цел и читается, журнал получил запись, каталог обновлён.
    ZT_TRUE("rewriteNote: " + s(error),
            storage.rewriteNote(noteId, [](zametti::ZNote& note) {
                note.doc().setTitle(QStringLiteral("Третья"));
                note.setParentId(QString());
            }, rules(), &error));
    ZT_EQ("заголовок после правки", "Третья", s(storage.info(noteId)->title()));
    ZT_TRUE("родитель снят", storage.info(noteId)->parent().isEmpty());
    {
        zametti::journal::Journal journal;
        zametti::ZNoteHistory history = storage.historyOf(noteId, rules());
        ZT_TRUE("журнал читается", history.read(&journal, &error));
        ZT_TRUE("в журнале есть запись о правке", !journal.entries.isEmpty());
    }
    ZT_TRUE("та же правка второй раз — не ошибка (файл не изменился)",
            storage.rewriteNote(noteId, [](zametti::ZNote& note) {
                note.doc().setTitle(QStringLiteral("Третья"));
            }, rules(), &error));

    // Файл унесли — из каталога уходит.
    ZT_TRUE("файл удалён", QFile::remove(notePath));
    ZT_TRUE("refreshNote пропавшей — ложь", !storage.refreshNote(noteId));
    ZT_TRUE("и в каталоге её нет", !storage.has(noteId));
    ZT_TRUE("правка пропавшей — ложь с объяснением",
            !storage.rewriteNote(noteId, [](zametti::ZNote&) {}, rules(), &error) &&
                !error.isEmpty());

    // Замок: одно хранилище — одна программа. Второй объект на том же корне
    // (как второй процесс) замок не получает и знает, кто держит; --unlock
    // снимает руками; после этого замок берётся снова.
    {
        const ZStorage::LockReport first = storage.lock();
        ZT_TRUE("замок взят", first.locked && storage.isLocked());
        ZStorage rival(root);
        const ZStorage::LockReport second = rival.lock();
        ZT_TRUE("второй на том же корне замка не получил", !second.locked);
        ZT_TRUE("и знает, кто держит (мы сами)",
                second.holderPid == QCoreApplication::applicationPid());
        const ZStorage::LockReport freed = rival.forceUnlock();
        ZT_TRUE("--unlock отчитался, кто держал", freed.note.contains(QStringLiteral("pid")));
        ZT_TRUE("после снятия замок берётся", rival.lock().locked);
    }

    // Не хранилище: каталог пуст, журнала нет, ничего не падает.
    ZStorage plain(home.path());
    ZT_TRUE("каталог — не хранилище", !plain.isStore());
    plain.reload();
    ZT_EQ("каталог пуст", n(0), n(plain.count()));
    // Журнал по id даётся у любого каталога с корнем (так живут наборы на
    // временном каталоге); нет корня — нет журнала.
    ZT_TRUE("журнала без корня нет", !ZStorage(QString()).historyOf(QStringLiteral("x"), rules()).available());
}

// Операции — одна точка правды: создать, архивировать (с поддеревом), вернуть,
// удалить насовсем; вопросы к каталогу без диска.
void checkOperations() {
    QTemporaryDir home;
    const QString root = home.path() + QStringLiteral("/store");
    QString error;
    ZT_TRUE("хранилище заведено", zametti::store::initStore(root, &error));
    ZStorage storage(root);
    storage.reload();

    const QString folder = storage.createNote(QString(), true, &error);
    ZT_TRUE("папка создана: " + s(error), !folder.isEmpty());
    ZT_TRUE("и она папка в каталоге", storage.isFolder(folder));
    ZT_EQ("с именем по умолчанию", "Новая папка", s(storage.titleOf(folder)));
    const QString inner = storage.createNote(folder, false, &error);
    ZT_TRUE("заметка в папке", !inner.isEmpty() && storage.info(inner)->parent() == folder);
    ZT_TRUE("пустая заметка пуста", storage.isEmptyNote(inner));
    ZT_TRUE("папка с ребёнком не пуста", !storage.isEmptyNote(folder));
    ZT_EQ("дети папки", "1", n(storage.childrenOf(folder).size()));
    ZT_EQ("потомки папки", "1", n(storage.descendantsOf(folder).size()));
    // Родитель в архиве — заметка идёт в корень.
    ZT_TRUE("рецепт: правка текста заметки",
            storage.rewriteNote(inner, [](zametti::ZNote& note) {
                note.doc().setTitle(QStringLiteral("Внутренняя"));
            }, rules(), &error));

    QStringList failed;
    ZT_TRUE("папка убрана в архив: " + s(failed.join(QStringLiteral("; "))),
            storage.archive(folder, rules(), &failed));
    ZT_TRUE("папка в архиве", storage.inArchive(folder));
    ZT_TRUE("и её ребёнок — по цепочке и сам", storage.inArchive(inner) && storage.info(inner)->archived());
    const QString stray = storage.createNote(folder, false, &error);
    ZT_TRUE("создание в архивной папке — в корень", !stray.isEmpty() && storage.info(stray)->parent().isEmpty());

    ZT_TRUE("возврат из архива", storage.restore(folder, &failed) && failed.isEmpty());
    ZT_TRUE("папка вернулась", !storage.inArchive(folder));
    ZT_TRUE("ребёнок вернулся туда же с текстом", !storage.inArchive(inner) &&
                                                       storage.info(inner)->parent() == folder &&
                                                       storage.info(inner)->title() == QStringLiteral("Внутренняя"));

    ZT_TRUE("удалить насовсем пустую", storage.remove(stray, &error));
    ZT_TRUE("её нет в каталоге", !storage.has(stray));
    ZT_TRUE("и файла нет", !QFile::exists(storage.pathOf(stray)));
    ZT_TRUE("удалить несуществующую — ложь", !storage.remove(stray, &error) && !error.isEmpty());

    // Импорт чужого .md.
    const QString foreign = home.path() + QStringLiteral("/чужая.md");
    {
        QFile f(foreign);
        ZT_TRUE("чужая записана", f.open(QIODevice::WriteOnly));
        f.write("# Чужая\n\nтекст\n");
    }
    const QString imported = storage.importNote(folder, foreign, &error);
    ZT_TRUE("импорт: " + s(error), !imported.isEmpty());
    ZT_EQ("импортированная в каталоге", "Чужая", s(storage.titleOf(imported)));
    ZT_TRUE("источник цел", QFile::exists(foreign));

    // Миграции идемпотентны: пустой ход ничего не сообщает.
    ZT_TRUE("повторная миграция молчит", storage.migrate().isEmpty());
}

}  // namespace

TEST(ZStorage, All) {
    checkCatalog();
    checkOperations();
    EXPECT_EQ(0, zt::freshFailures());
}
