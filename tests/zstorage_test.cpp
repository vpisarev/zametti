// ZStorage — хранилище как объект: каталог по id, путь и журнал по id, правка
// шапки закрытой заметки штатным путём записи.
//
// Тесты как критерий дизайна: каталог совпадает с диском после reload и после
// refreshNote; исчезнувший файл уходит из каталога; rewriteNote пишет так же,
// как редактор (самопроверка, атомарно, шаг журнала) и обновляет каталог; вне
// хранилища всё пусто и ничего не падает.

#include "zstorage.h"
#include "store.h"
#include "journal.h"
#include "exif.h"
#include "jxl_encoder.h"
#include "image_read.h"

#include <QImage>
#include "test_util.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <string>

namespace {

using zametti::ZStorage;

std::string s(const QString& q) { return q.toStdString(); }

QString readFile(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    return QString::fromUtf8(f.readAll());
}
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
        zametti::journal::ZJournal journal;
        zametti::ZNoteHistory history = storage.historyOf(noteId, rules());
        ZT_TRUE("журнал читается", history.read(&journal, &error));
        ZT_TRUE("в журнале есть запись о правке", !journal.isEmpty());
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
    ZT_EQ("с именем по умолчанию", "New folder", s(storage.titleOf(folder)));
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
    // ТЕЛО ЗАМЕТКИ ОБЯЗАНО ПЕРЕЖИТЬ АРХИВАЦИЮ, и спрашивается это на БОЕВОМ
    // пути окна — ZStorage::archive, а не store::archiveNote напрямую: у папки
    // и у заметки внутри неё пути разные, и проверять надо тот, которым ходит
    // человек.
    const QString innerBefore = readFile(storage.pathOf(inner));
    ZT_TRUE("папка убрана в архив: " + s(failed.join(QStringLiteral("; "))),
            storage.archive(folder, rules(), &failed));
    const QString innerAfter = readFile(storage.pathOf(inner));
    ZT_TRUE("тело заметки в архивной папке на месте: " + s(innerAfter),
            innerAfter.contains(QStringLiteral("Внутренняя")));
    ZT_EQ("и отличается от прежнего ровно пометкой",
          s(QString(innerBefore).replace(QStringLiteral("-->"),
                                         QStringLiteral("archived: yes\n-->"))),
          s(innerAfter));
    ZT_TRUE("папка в архиве", storage.inArchive(folder));
    ZT_TRUE("и её ребёнок — по цепочке и сам", storage.inArchive(inner) && storage.info(inner)->archived());
    const QString stray = storage.createNote(folder, false, &error);
    ZT_TRUE("создание в архивной папке — в корень", !stray.isEmpty() && storage.info(stray)->parent().isEmpty());

    ZT_TRUE("возврат из архива", storage.restore(folder, &failed) && failed.isEmpty());
    ZT_TRUE("папка вернулась", !storage.inArchive(folder));
    ZT_TRUE("ребёнок вернулся туда же с текстом", !storage.inArchive(inner) &&
                                                       storage.info(inner)->parent() == folder &&
                                                       storage.info(inner)->title() == QStringLiteral("Внутренняя"));

    ZT_TRUE("удалить насовсем пустую", storage.remove(stray, zametti::ImportLimits{}, &error));
    ZT_TRUE("её нет в каталоге", !storage.has(stray));
    ZT_TRUE("и файла нет", !QFile::exists(storage.pathOf(stray)));
    ZT_TRUE("удалить несуществующую — ложь", !storage.remove(stray, zametti::ImportLimits{}, &error) && !error.isEmpty());

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

// СИГНАЛЫ КАТАЛОГА: структурная новость — одна на операцию, какой бы длинной
// она ни была (архив папки с детьми — одна перестройка дерева, а не по числу
// перезаписей); правка на месте — noteChanged без catalogChanged.
// УДАЛЕНИЕ ПАПКИ УНОСИТ ПОДДЕРЕВО. Прежде уносило только её файл, а дети
// оставались с оборванным parent — и уезжали в бюро находок при следующем
// открытии. Заодно: путь удаления один для архивных и живых, журнал у всех
// переживает удаление надгробием.
void checkDeleteCascade() {
    QTemporaryDir home;
    const QString root = home.path() + QStringLiteral("/store");
    QString error;
    ZT_TRUE("хранилище заведено", zametti::store::initStore(root, &error));
    ZStorage storage(root);
    storage.reload();

    const QString folder = storage.createNote(QString(), true, &error);
    const QString child = storage.createNote(folder, false, &error);
    const QString grand = storage.createNote(child, false, &error);
    ZT_TRUE("дерево создано", !folder.isEmpty() && !child.isEmpty() && !grand.isEmpty());

    // Внук уходит в архив — путь удаления обязан быть одним для обоих.
    QStringList failed;
    ZT_TRUE("внук в архиве", storage.archive(grand, rules(), &failed) && failed.isEmpty());

    const QString childLog = zametti::journal::History(root).pathFor(child);
    ZT_TRUE("удаление папки прошло", storage.remove(folder, zametti::ImportLimits{}, &error));

    ZT_TRUE("папки нет в каталоге", !storage.has(folder));
    ZT_TRUE("ребёнка тоже", !storage.has(child));
    ZT_TRUE("и внука", !storage.has(grand));
    ZT_TRUE("файла ребёнка нет", !QFile::exists(storage.pathOf(child)));
    ZT_TRUE("файла внука нет", !QFile::exists(storage.pathOf(grand)));

    // ЖУРНАЛ ПЕРЕЖИВАЕТ УДАЛЕНИЕ — и у архивного внука тоже: надгробие обязано
    // доехать до других устройств, иначе синк привезёт заметку обратно.
    ZT_TRUE("журнал ребёнка на месте", QFile::exists(childLog));
    zametti::journal::ZJournal read;
    zametti::journal::History history(root);
    ZT_TRUE("журнал внука читается", history.read(grand, &read, &error));
    ZT_TRUE("и голова у него — надгробие",
            !read.isEmpty() && read.at(read.headIndex()).kind() == zametti::journal::Kind::Tombstone);
}

// ВЛОЖЕНИЕ ХОРОНИТСЯ, А НЕ СТИРАЕТСЯ. Прежде оно уходило в мусорку ОС — и
// удаление не доезжало никуда: «файла нет» не расскажешь другому устройству.
// Теперь на месте файла остаётся посмертное превью с меткой.
void checkAttachmentBurial() {
    QTemporaryDir home;
    const QString root = home.path() + QStringLiteral("/store");
    QString error;
    ZT_TRUE("хранилище заведено", zametti::store::initStore(root, &error));
    ZStorage storage(root);
    storage.reload();

    // Картинка покрупнее бюджета посмертного превью, чтобы было чему ужиматься.
    // Картинка НЕ однотонная: ровная заливка сжимается в двести байт, и тогда
    // превью с метаданными выходит больше исходника — сравнение потеряло бы
    // смысл, а не поймало бы беду.
    QImage picture(800, 600, QImage::Format_RGB32);
    for (int y = 0; y < picture.height(); ++y)
        for (int x = 0; x < picture.width(); ++x)
            picture.setPixel(x, y, qRgb((x * 255) / 799, (y * 255) / 599, (x ^ y) & 0xff));
    zametti::EncodeOptions options;
    options.quality = 90;
    options.effort = 3;
    const QByteArray jxl = zametti::encodeJxl(picture, options, {}, &error);
    ZT_TRUE("картинка закодирована", !jxl.isEmpty());
    const QString attachment = QStringLiteral("01jd7f0kq2m8xa.jxl");
    {
        QFile f(QDir(root).filePath(attachment));
        ZT_TRUE("вложение записано", f.open(QIODevice::WriteOnly));
        f.write(jxl);
    }
    const qint64 fat = QFileInfo(QDir(root).filePath(attachment)).size();

    const QString note = storage.createNote(QString(), false, &error);
    ZT_TRUE("заметка создана", !note.isEmpty());
    {
        QFile f(storage.pathOf(note));
        ZT_TRUE("заметка открыта", f.open(QIODevice::Append));
        f.write("# Со снимком\n\n![вид](01jd7f0kq2m8xa.jxl#w=600)\n");
    }
    storage.refreshNote(note);

    zametti::ImportLimits limits;
    limits.maxSize = 100;
    limits.quality = 90;
    ZT_TRUE("удаление прошло", storage.remove(note, limits, &error));

    const QString buried = QDir(root).filePath(attachment);
    ZT_TRUE("файл вложения на месте", QFile::exists(buried));
    ZT_TRUE("и файл меньше исходного: " + n(QFileInfo(buried).size()) + " против " + n(fat),
            QFileInfo(buried).size() < fat);

    QFile f(buried);
    ZT_TRUE("превью читается", f.open(QIODevice::ReadOnly));
    const QByteArray bytes = f.readAll();
    const zametti::ImageMeta meta =
        zametti::readImageMeta(std::string_view(bytes.constData(), size_t(bytes.size())));
    ZT_TRUE("и помечено удалённым", zametti::xmpZamettiDeleted(meta.xmp));
    // Главное — не байты файла, а пиксели: превью обязано влезать в бюджет S².
    const QImage back = zametti::decodeImage(bytes);
    ZT_TRUE("и влезает в бюджет: " + n(back.width()) + "×" + n(back.height()),
            !back.isNull() && back.width() * back.height() <= 101 * 101);

    // ИДЕМПОТЕНТНОСТЬ: повторные похороны не трогают файл ни байтом — иначе
    // ревизия росла бы вечно, а картинка ужималась заново.
    const QByteArray was = bytes;
    QString why;
    ZT_TRUE("повторные похороны молчат",
            zametti::store::retireAttachmentFile(root, attachment, limits, &why));
    QFile again(buried);
    ZT_TRUE("файл читается", again.open(QIODevice::ReadOnly));
    ZT_TRUE("и не изменился ни байтом", again.readAll() == was);
}

// КАРТИНКА, ПОДЕЛЁННАЯ ДВУМЯ ЗАМЕТКАМИ, переживает удаление одной из них — и
// это ровно тот случай, который ломался, пока архивация срезала тело: у
// архивной заметки в файле не оставалось ссылок, она картинку «не держала», и
// удаление СОСЕДНЕЙ уносило снимок, который на самом деле был нужен.
void checkSharedAttachmentSurvives() {
    QTemporaryDir home;
    const QString root = home.path() + QStringLiteral("/store");
    QString error;
    ZT_TRUE("хранилище заведено", zametti::store::initStore(root, &error));
    ZStorage storage(root);
    storage.reload();

    QImage picture(400, 300, QImage::Format_RGB32);
    for (int y = 0; y < picture.height(); ++y)
        for (int x = 0; x < picture.width(); ++x)
            picture.setPixel(x, y, qRgb((x * 255) / 399, (y * 255) / 299, (x ^ y) & 0xff));
    zametti::EncodeOptions options;
    options.quality = 90;
    options.effort = 3;
    const QByteArray jxl = zametti::encodeJxl(picture, options, {}, &error);
    const QString attachment = QStringLiteral("01jd7f0kq2m8xa.jxl");
    {
        QFile f(QDir(root).filePath(attachment));
        ZT_TRUE("вложение записано", f.open(QIODevice::WriteOnly));
        f.write(jxl);
    }
    const QByteArray was = jxl;

    const auto noteWithPicture = [&](const QString& title) {
        const QString id = storage.createNote(QString(), false, &error);
        QFile f(storage.pathOf(id));
        ZT_TRUE("заметка открыта на дозапись", f.open(QIODevice::Append));
        f.write(("# " + title + "\n\n![вид](01jd7f0kq2m8xa.jxl)\n").toUtf8());
        f.close();
        storage.refreshNote(id);
        return id;
    };
    const QString a = noteWithPicture(QStringLiteral("Заметка A"));
    const QString b = noteWithPicture(QStringLiteral("Заметка B"));
    ZT_TRUE("обе заметки заведены", !a.isEmpty() && !b.isEmpty());

    // A уходит в архив, потом удаляется насовсем.
    QStringList failed;
    ZT_TRUE("A в архиве", storage.archive(a, rules(), &failed) && failed.isEmpty());
    zametti::ImportLimits limits;
    limits.maxSize = 100;
    limits.quality = 90;
    ZT_TRUE("A удалена насовсем", storage.remove(a, limits, &error));

    // КАРТИНКА ЦЕЛА И НЕ ПОХОРОНЕНА: её держит живая B.
    const QString path = QDir(root).filePath(attachment);
    ZT_TRUE("файл вложения на месте", QFile::exists(path));
    QFile f(path);
    ZT_TRUE("и читается", f.open(QIODevice::ReadOnly));
    const QByteArray now = f.readAll();
    ZT_TRUE("и не тронут ни байтом: она нужна заметке B", now == was);
    const zametti::ImageMeta meta =
        zametti::readImageMeta(std::string_view(now.constData(), size_t(now.size())));
    ZT_TRUE("и не помечена удалённой", !zametti::xmpZamettiDeleted(meta.xmp));

    // А когда уйдёт и B — вот тогда похороны.
    ZT_TRUE("B в архиве", storage.archive(b, rules(), &failed) && failed.isEmpty());
    ZT_TRUE("B удалена насовсем", storage.remove(b, limits, &error));
    QFile after(path);
    ZT_TRUE("файл всё ещё на месте", after.open(QIODevice::ReadOnly));
    const QByteArray buried = after.readAll();
    const zametti::ImageMeta grave =
        zametti::readImageMeta(std::string_view(buried.constData(), size_t(buried.size())));
    ZT_TRUE("теперь она похоронена", zametti::xmpZamettiDeleted(grave.xmp));
    ZT_TRUE("и ужалась", buried.size() < was.size());
}

void checkSignals() {
    QTemporaryDir home;
    const QString root = home.path() + QStringLiteral("/store");
    QString error;
    ZT_TRUE("хранилище заведено", zametti::store::initStore(root, &error));
    ZStorage storage(root);
    int catalog = 0;
    QStringList rows;
    QObject::connect(&storage, &ZStorage::catalogChanged, [&catalog] { ++catalog; });
    QObject::connect(&storage, &ZStorage::noteChanged, [&rows](const QString& id) { rows << id; });

    storage.reload();
    ZT_EQ("reload — одна структурная новость", "1", n(catalog));
    catalog = 0;

    const QString folder = storage.createNote(QString(), true, &error);
    ZT_EQ("папка создана — одна новость (две записи внутри)", "1", n(catalog));
    catalog = 0;
    const QString a = storage.createNote(folder, false, &error);
    const QString b = storage.createNote(folder, false, &error);
    ZT_EQ("две заметки — две новости", "2", n(catalog));
    catalog = 0;
    rows.clear();

    ZT_TRUE("переименование", storage.rename(a, QStringLiteral("Альфа"), rules(), &error));
    ZT_EQ("переименование — новость о строке", "1", n(rows.size()));
    ZT_TRUE("о той самой", rows.value(0) == a);
    ZT_EQ("и не о каталоге", "0", n(catalog));
    rows.clear();

    ZT_TRUE("перенос в корень", storage.move(a, QString(), rules(), &error));
    ZT_EQ("перенос — структурная новость", "1", n(catalog));
    ZT_EQ("и не строка", "0", n(rows.size()));
    catalog = 0;

    ZT_TRUE("метка порядка", storage.setSortMark(folder, zametti::SortOrder{zametti::SortKey::Name, true},
                                                rules(), &error));
    ZT_EQ("метка — структурная новость (дерево пересортируется)", "1", n(catalog));
    catalog = 0;

    QStringList failed;
    ZT_TRUE("папка с ребёнком в архив", storage.archive(folder, rules(), &failed) && failed.isEmpty());
    ZT_EQ("архив поддерева — ОДНА новость", "1", n(catalog));
    catalog = 0;
    ZT_TRUE("возврат", storage.restore(folder, &failed) && failed.isEmpty());
    ZT_EQ("возврат поддерева — одна новость", "1", n(catalog));
    catalog = 0;
    ZT_TRUE("удаление", storage.remove(b, zametti::ImportLimits{}, &error));
    ZT_EQ("удаление — одна новость", "1", n(catalog));
    catalog = 0;

    // Перечитать заметку, которая на месте, — новость о строке; исчезнувшую —
    // структурная (строки в дереве больше нет).
    rows.clear();
    ZT_TRUE("refreshNote живой", storage.refreshNote(a));
    ZT_EQ("живая — строка", "1", n(rows.size()));
    ZT_EQ("живая — не каталог", "0", n(catalog));
    QFile::remove(storage.pathOf(a));
    ZT_TRUE("refreshNote исчезнувшей — ложь", !storage.refreshNote(a));
    ZT_EQ("исчезнувшая — структурная новость", "1", n(catalog));
    ZT_TRUE("и её нет в каталоге", !storage.has(a));
}

}  // namespace

TEST(ZStorage, All) {
    checkCatalog();
    checkOperations();
    checkDeleteCascade();
    checkAttachmentBurial();
    checkSharedAttachmentSurvives();
    checkSignals();
    EXPECT_EQ(0, zt::freshFailures());
}
