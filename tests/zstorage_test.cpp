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
            storage.rewriteNote(noteId, [](zametti::ZDocument& doc) {
                doc.setTitle(QStringLiteral("Третья"));
                doc.setParentId(QString());
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
            storage.rewriteNote(noteId, [](zametti::ZDocument& doc) {
                doc.setTitle(QStringLiteral("Третья"));
            }, rules(), &error));

    // Файл унесли — из каталога уходит.
    ZT_TRUE("файл удалён", QFile::remove(notePath));
    ZT_TRUE("refreshNote пропавшей — ложь", !storage.refreshNote(noteId));
    ZT_TRUE("и в каталоге её нет", !storage.has(noteId));
    ZT_TRUE("правка пропавшей — ложь с объяснением",
            !storage.rewriteNote(noteId, [](zametti::ZDocument&) {}, rules(), &error) &&
                !error.isEmpty());

    // Не хранилище: каталог пуст, журнала нет, ничего не падает.
    ZStorage plain(home.path());
    ZT_TRUE("каталог — не хранилище", !plain.isStore());
    plain.reload();
    ZT_EQ("каталог пуст", n(0), n(plain.count()));
    ZT_TRUE("журнала нет", !plain.historyOf(QStringLiteral("x"), rules()).available());
}

}  // namespace

TEST(ZStorage, All) {
    checkCatalog();
    EXPECT_EQ(0, zt::freshFailures());
}
