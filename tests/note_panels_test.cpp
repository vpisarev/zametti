// Левая и средняя колонки как объект (NotePanels) — против живого хранилища.
//
// Что стережётся. Панели больше не обновляет окно после каждой операции: о
// переменах говорит хранилище (ZStorage::catalogChanged), дерево строится по
// сигналу, раскрытость и курсор бережёт само дерево через сброс модели, список
// наполняется по текущей папке. Значит: создать / переименовать / перенести /
// убрать в архив / вернуть — и панели ОБЯЗАНЫ показать это сами, ничего не
// потеряв: ни раскрытых веток, ни выбранной папки, ни выделенной заметки. Плюс
// два выделения — первичное (курсор: папка, по которой ткнули; она задаёт
// среднюю колонку) и вторичное (папка открытой заметки, пунктирная рамка; показ
// заметки НЕ двигает курсор и НЕ меняет ни состав, ни порядок списка — решение
// владельца) — и намерения человека сигналами.

#include "editor_widget.h"
#include "note_panels.h"
#include "store.h"
#include "test_util.h"
#include "testdata.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

#include <cstdio>
#include <string>
#include <vector>

namespace {

using zametti::NotePanels;
using zametti::ZStorage;

std::string s(const QString& q) { return q.toStdString(); }
std::string n(long long v) { return std::to_string(v); }

zametti::history::Rules rules() { return zametti::history::Rules{}; }

QModelIndex byTitle(const zametti::NoteTreeModel& model, const QModelIndex& parent,
                    const QString& title) {
    for (int row = 0; row < model.rowCount(parent); ++row) {
        const QModelIndex at = model.index(row, 0, parent);
        if (model.titleOf(at) == title) return at;
        const QModelIndex deeper = byTitle(model, at, title);
        if (deeper.isValid()) return deeper;
    }
    return {};
}

QStringList listTitles(const zametti::NoteListModel& list) {
    QStringList out;
    for (int row = 0; row < list.rowCount(); ++row)
        out << list.data(list.index(row, 0), zametti::NoteListModel::TitleRole).toString();
    return out;
}

}  // namespace

static int ztRunSuite(int, char**) {
    QTemporaryDir home;
    const QString root = home.path() + QStringLiteral("/store");
    QString error;
    ZT_TRUE("хранилище заведено: " + s(error), zametti::store::initStore(root, &error));

    // Хранилище: Работа ⊃ Проекты ⊃ {Альфа, Бета}; Дом ⊃ {Гамма}; корневая Дельта.
    auto storage = std::make_shared<ZStorage>(root);
    storage->reload();
    const QString work = storage->createNote(QString(), true, &error);
    const QString home_ = storage->createNote(QString(), true, &error);
    const QString projects = storage->createNote(work, true, &error);
    ZT_TRUE("папки созданы", !work.isEmpty() && !home_.isEmpty() && !projects.isEmpty());
    ZT_TRUE("имена папок", storage->rename(work, QStringLiteral("Работа"), rules(), &error) &&
                               storage->rename(home_, QStringLiteral("Дом"), rules(), &error) &&
                               storage->rename(projects, QStringLiteral("Проекты"), rules(), &error));
    const QString alpha = storage->createNote(projects, false, &error);
    const QString beta = storage->createNote(projects, false, &error);
    const QString gamma = storage->createNote(home_, false, &error);
    const QString delta = storage->createNote(QString(), false, &error);
    ZT_TRUE("заметки созданы", !alpha.isEmpty() && !beta.isEmpty() && !gamma.isEmpty() && !delta.isEmpty());
    ZT_TRUE("имена заметок",
            storage->rename(alpha, QStringLiteral("Альфа"), rules(), &error) &&
                storage->rename(beta, QStringLiteral("Бета"), rules(), &error) &&
                storage->rename(gamma, QStringLiteral("Гамма"), rules(), &error) &&
                storage->rename(delta, QStringLiteral("Дельта"), rules(), &error));

    NotePanels panels(storage);
    zametti::NoteTreeModel& model = panels.model();
    zametti::NoteTreeView& tree = panels.tree();
    zametti::NoteListModel& list = panels.list();
    ZT_TRUE("это хранилище", panels.isStore());

    // Что панели просят открыть.
    QStringList chosen;
    QObject::connect(&panels, &NotePanels::noteChosen, &panels,
                     [&chosen](const QString& file, bool) { chosen << file; });
    int sortShown = 0;
    QObject::connect(&panels, &NotePanels::sortShown, &panels,
                     [&sortShown](zametti::SortOrder, bool) { ++sortShown; });

    tree.setAttribute(Qt::WA_DontShowOnScreen);
    tree.resize(300, 400);
    tree.show();
    panels.listView().setAttribute(Qt::WA_DontShowOnScreen);
    panels.listView().resize(300, 400);
    panels.listView().show();

    // --- выбор папки: порядок, список, первая заметка ------------------------
    const QModelIndex projectsRow = byTitle(model, QModelIndex(), QStringLiteral("Проекты"));
    ZT_TRUE("папка «Проекты» в дереве", projectsRow.isValid());
    tree.setCurrentIndex(projectsRow);
    QCoreApplication::processEvents();
    {
        QStringList titles = listTitles(list);
        titles.sort();
        ZT_EQ("список — заметки папки", "Альфа,Бета", s(titles.join(QLatin1Char(','))));
    }
    ZT_EQ("выбор папки просит открыть первую заметку", "1", n(chosen.size()));
    ZT_TRUE("порядок показан кнопкам", sortShown > 0);
    const QString firstOpened = chosen.value(0);
    // Открытие состоялось (так сделало бы окно): панели узнают о нём прямо.
    panels.setCurrentNote(firstOpened);
    panels.showNote(firstOpened);
    ZT_TRUE("курсор дерева остался на «Проекты» (заметку открыл выбор папки)",
            tree.currentIndex() == byTitle(model, QModelIndex(), QStringLiteral("Проекты")));
    ZT_EQ("в списке выделена открытая", s(firstOpened),
          s(list.pathAt(panels.listView().currentIndex())));

    // --- операции хранилища: панели догоняют сами ------------------------------
    tree.expand(byTitle(model, QModelIndex(), QStringLiteral("Работа")));
    const QString newId = storage->createNote(projects, false, &error);
    ZT_TRUE("создана", !newId.isEmpty() && storage->rename(newId, QStringLiteral("Новая"), rules(), &error));
    QCoreApplication::processEvents();
    ZT_TRUE("после создания курсор дерева там же",
            model.titleOf(tree.currentIndex()) == QStringLiteral("Проекты"));
    ZT_TRUE("«Работа» осталась раскрытой",
            tree.isExpanded(byTitle(model, QModelIndex(), QStringLiteral("Работа"))));
    ZT_TRUE("новая заметка в списке", listTitles(list).contains(QStringLiteral("Новая")));
    ZT_EQ("открытая осталась выделенной", s(firstOpened),
          s(list.pathAt(panels.listView().currentIndex())));
    ZT_EQ("операции хранилища ничего не открывают", "1", n(chosen.size()));

    // Переименование — строка на месте.
    ZT_TRUE("переименовали", storage->rename(newId, QStringLiteral("Новейшая"), rules(), &error));
    QCoreApplication::processEvents();
    ZT_TRUE("список показал новое имя", listTitles(list).contains(QStringLiteral("Новейшая")));

    // Перенос в «Дом» — из списка «Проектов» ушла.
    ZT_TRUE("перенесли", storage->move(newId, home_, rules(), &error));
    QCoreApplication::processEvents();
    ZT_TRUE("из списка ушла", !listTitles(list).contains(QStringLiteral("Новейшая")));
    ZT_TRUE("курсор дерева всё там же",
            model.titleOf(tree.currentIndex()) == QStringLiteral("Проекты"));

    // Архив папки «Дом» — папка ушла из живого дерева, курсор и раскрытость целы.
    QStringList failed;
    ZT_TRUE("дом в архив", storage->archive(home_, rules(), &failed) && failed.isEmpty());
    QCoreApplication::processEvents();
    ZT_TRUE("«Работа» всё ещё раскрыта",
            tree.isExpanded(byTitle(model, QModelIndex(), QStringLiteral("Работа"))));
    ZT_TRUE("курсор на «Проекты»", model.titleOf(tree.currentIndex()) == QStringLiteral("Проекты"));
    ZT_TRUE("«Дом» в архиве по модели", model.inArchiveId(home_));

    // Возврат из архива и показ, куда вернулась: ВТОРИЧНЫМ выделением. Курсор
    // остаётся на «Проектах», список — их (человек смотрит свою папку), а «Дом»
    // получает пунктирную рамку и раскрытых предков.
    ZT_TRUE("вернули", storage->restore(home_, &failed) && failed.isEmpty());
    QCoreApplication::processEvents();
    const QString gammaFile = storage->pathOf(gamma);
    const QStringList listBefore = listTitles(list);
    panels.setCurrentNote(gammaFile);
    panels.showNote(gammaFile);
    ZT_TRUE("курсор дерева остался на «Проекты»",
            model.titleOf(tree.currentIndex()) == QStringLiteral("Проекты"));
    ZT_EQ("состав списка не изменился", s(listBefore.join(QLatin1Char('|'))),
          s(listTitles(list).join(QLatin1Char('|'))));
    ZT_EQ("вторичное — на «Дом»", s(storage->pathOf(home_)), s(model.secondaryPath()));
    ZT_TRUE("роль вторичного у строки «Дом»",
            model.data(byTitle(model, QModelIndex(), QStringLiteral("Дом")),
                       zametti::NoteTreeModel::SecondaryRole).toBool());

    // --- порядок папки принадлежит первичному выделению --------------------------
    // Человек выбрал «Работу» с меткой порядка по имени; показ заметки из «Дома»
    // (без метки) порядок списка не трогает — ни состав, ни сортировку.
    ZT_TRUE("метка порядка на «Работе»",
            storage->setSortMark(work, zametti::SortOrder{zametti::SortKey::Name, false}, rules(), &error));
    QCoreApplication::processEvents();
    const QModelIndex workRow = byTitle(model, QModelIndex(), QStringLiteral("Работа"));
    tree.setCurrentIndex(workRow);   // человек выбрал «Работу»: список — всё поддерево
    QCoreApplication::processEvents();
    chosen.clear();
    ZT_EQ("список идёт порядком «Работы» (по имени)", n(int(zametti::SortKey::Name)),
          n(int(list.sortOrder().key)));
    const QStringList workList = listTitles(list);
    panels.setCurrentNote(gammaFile);
    panels.showNote(gammaFile);
    ZT_TRUE("показ заметки из «Дома» курсор не двигает",
            model.titleOf(tree.currentIndex()) == QStringLiteral("Работа"));
    ZT_EQ("порядок списка остался порядком «Работы»", n(int(zametti::SortKey::Name)),
          n(int(list.sortOrder().key)));
    ZT_EQ("и состав тот же", s(workList.join(QLatin1Char('|'))), s(listTitles(list).join(QLatin1Char('|'))));
    ZT_EQ("показ ничего не открывает", "0", n(chosen.size()));

    // --- порядок корня: сброс модели, курсор и раскрытость целы -----------------
    tree.expand(workRow);
    panels.setRootSort(zametti::SortOrder{zametti::SortKey::Created, true});
    QCoreApplication::processEvents();
    ZT_TRUE("после смены порядка курсор на «Работе»",
            model.titleOf(tree.currentIndex()) == QStringLiteral("Работа"));
    ZT_TRUE("«Работа» раскрыта и после смены порядка",
            tree.isExpanded(byTitle(model, QModelIndex(), QStringLiteral("Работа"))));
    ZT_EQ("порядок списка — метки «Работы», не корня", n(int(zametti::SortKey::Name)),
          n(int(list.sortOrder().key)));

    // --- удаление насовсем: строка ушла, ничего не открыто ------------------------
    ZT_TRUE("удалили Гамму", storage->remove(gamma, &error));
    QCoreApplication::processEvents();
    ZT_TRUE("Гаммы нет в списке", !listTitles(list).contains(QStringLiteral("Гамма")));
    ZT_EQ("удаление ничего не открывает", "0", n(chosen.size()));

    // --- сторож каталога: чужой файл появился — дерево узнало ----------------------
    storage->setWatching(true);
    {
        QFile f(root + QStringLiteral("/01zzzzzzzzzzzz.md"));
        ZT_TRUE("чужая записана", f.open(QIODevice::WriteOnly));
        f.write("<!-- zametti\n-->\n\n# Снаружи\n");
    }
    bool seen = false;
    for (int i = 0; i < 40 && !seen; ++i) {
        QTest::qWait(50);
        seen = model.hasNote(QStringLiteral("01zzzzzzzzzzzz"));
    }
    ZT_TRUE("сторож перечитал каталог, заметка в дереве", seen);
    ZT_TRUE("и курсор дерева цел", model.titleOf(tree.currentIndex()) == QStringLiteral("Работа"));

    return zt::report("панели заметок");
}

// --- то же на КОПИИ ХРАНИЛИЩА ВЛАДЕЛЬЦА ------------------------------------------
//
// Синтетика повторяет строение и не повторяет ни размера, ни шапок, ни соседей.
// Копия — в каталог набора; оригинал (.testdata/owner-copy) только читается.
static bool copyDir(const QString& from, const QString& to) {
    QDir().mkpath(to);
    for (const QFileInfo& e :
         QDir(from).entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden)) {
        const QString target = to + QLatin1Char('/') + e.fileName();
        if (e.isDir()) {
            if (!copyDir(e.absoluteFilePath(), target)) return false;
        } else if (!QFile::copy(e.absoluteFilePath(), target)) {
            return false;
        }
    }
    return true;
}

static int ztRunOwnerCopy() {
    const QString source = zt::TestData::corpus(QStringLiteral("owner-copy"));
    if (source.isEmpty()) {
        std::printf("owner-copy: корпуса нет, акт пропущен\n");
        return 0;
    }
    const QString root = zt::TestData::outDir(QStringLiteral("note-panels")) + QStringLiteral("/store");
    if (!copyDir(source, root)) {
        std::printf("owner-copy: не скопировалось\n");
        return 1;
    }
    QFile::remove(root + QStringLiteral("/.zametti/store.lock"));

    auto storage = std::make_shared<ZStorage>(root);
    storage->reload();
    NotePanels panels(storage);
    zametti::NoteTreeModel& model = panels.model();
    zametti::NoteTreeView& tree = panels.tree();
    zametti::NoteListModel& list = panels.list();
    tree.setAttribute(Qt::WA_DontShowOnScreen);
    tree.resize(300, 600);
    tree.show();
    panels.listView().setAttribute(Qt::WA_DontShowOnScreen);
    panels.listView().resize(300, 600);
    panels.listView().show();
    QStringList chosen;
    QObject::connect(&panels, &NotePanels::noteChosen, &panels,
                     [&chosen](const QString& file, bool) { chosen << file; });

    // Первая живая папка с заметками (не архив), и в ней — первая заметка.
    QString folderId;
    for (const QString& id : storage->ids()) {
        if (!storage->isFolder(id) || storage->inArchive(id)) continue;
        bool hasNote = false;
        for (const QString& child : storage->childrenOf(id))
            if (!storage->isFolder(child) && !storage->inArchive(child)) hasNote = true;
        if (hasNote) { folderId = id; break; }
    }
    ZT_TRUE("в хранилище владельца есть живая папка с заметками", !folderId.isEmpty());
    if (folderId.isEmpty()) return zt::report("панели на копии владельца");
    const QString folderPath = storage->pathOf(folderId);
    const QModelIndex folderRow = model.indexForPath(folderPath);
    ZT_TRUE("папка в дереве", folderRow.isValid());
    tree.setCurrentIndex(folderRow);
    QCoreApplication::processEvents();
    const int rowsBefore = list.rowCount();
    ZT_TRUE("список папки не пуст", rowsBefore > 0);
    ZT_EQ("выбор папки открыл первую", "1", n(chosen.size()));
    const QString opened = chosen.value(0);
    panels.setCurrentNote(opened);
    panels.showNote(opened);
    const QStringList expandedBefore = panels.expandedDirs();
    ZT_TRUE("что-то раскрыто (предки папки)", !expandedBefore.isEmpty());

    QString error;
    const QString made = storage->createNote(folderId, false, &error);
    ZT_TRUE("создана в папке владельца: " + s(error), !made.isEmpty());
    ZT_TRUE("переименована", storage->rename(made, QStringLiteral("Проверочная"), rules(), &error));
    QCoreApplication::processEvents();
    ZT_EQ("список вырос на одну", n(rowsBefore + 1), n(list.rowCount()));
    ZT_TRUE("новая в списке под именем", listTitles(list).contains(QStringLiteral("Проверочная")));
    ZT_EQ("курсор дерева — та же папка", s(folderPath), s(tree.currentPath()));
    ZT_EQ("раскрытость та же", s(expandedBefore.join(QLatin1Char('|'))),
          s(panels.expandedDirs().join(QLatin1Char('|'))));
    ZT_EQ("выделена по-прежнему открытая", s(opened), s(list.pathAt(panels.listView().currentIndex())));

    // Перенос в корень — из списка папки ушла; архив — ушла из живого; возврат.
    ZT_TRUE("перенос в корень", storage->move(made, QString(), rules(), &error));
    QCoreApplication::processEvents();
    ZT_TRUE("после переноса в списке папки её нет", !listTitles(list).contains(QStringLiteral("Проверочная")));
    QStringList failed;
    ZT_TRUE("в архив", storage->archive(made, rules(), &failed) && failed.isEmpty());
    QCoreApplication::processEvents();
    ZT_TRUE("в архиве по модели", model.inArchiveId(made));
    ZT_EQ("курсор дерева цел и после архива", s(folderPath), s(tree.currentPath()));
    ZT_TRUE("вернули", storage->restore(made, &failed) && failed.isEmpty());
    QCoreApplication::processEvents();
    ZT_TRUE("вернулась из архива", !model.inArchiveId(made));
    panels.showNote(storage->pathOf(made));
    ZT_EQ("показ возвращённой: курсор дерева не сдвинут", s(folderPath), s(tree.currentPath()));
    ZT_EQ("вторичное — на корне (заметка вернулась в корень)",
          s(model.nodePath(model.indexForPath(model.nodePath(QModelIndex())))), s(model.secondaryPath()));
    ZT_EQ("операции сами ничего не открывали", "1", n(chosen.size()));

    return zt::report("панели на копии владельца");
}

// --- В СВЯЗКЕ С РЕДАКТОРОМ, как в окне ------------------------------------------
//
// Та самая проводка main.cpp: панели просят открыть — редактор открывает;
// редактор открыл — панели узнают прямо (setCurrentNote) и показывают
// очередью (showNote). Проверяется круг целиком: щелчок по папке → первая
// заметка открыта в редакторе → строка выделена, курсор дерева не уехал; операция
// хранилища ничего не переоткрывает; открытая заметка из другой папки — показана.
static int ztRunWired() {
    QTemporaryDir home;
    const QString root = home.path() + QStringLiteral("/store");
    QString error;
    ZT_TRUE("хранилище заведено", zametti::store::initStore(root, &error));
    auto storage = std::make_shared<ZStorage>(root);
    storage->reload();
    const QString a = storage->createNote(QString(), true, &error);
    const QString b = storage->createNote(QString(), true, &error);
    ZT_TRUE("папки", storage->rename(a, QStringLiteral("А"), rules(), &error) &&
                         storage->rename(b, QStringLiteral("Б"), rules(), &error));
    const QString a1 = storage->createNote(a, false, &error);
    const QString a2 = storage->createNote(a, false, &error);
    const QString b1 = storage->createNote(b, false, &error);
    ZT_TRUE("заметки", storage->rename(a1, QStringLiteral("А-1"), rules(), &error) &&
                           storage->rename(a2, QStringLiteral("А-2"), rules(), &error) &&
                           storage->rename(b1, QStringLiteral("Б-1"), rules(), &error));

    NotePanels panels(storage);
    zametti::NoteEditor editor;
    editor.setStorage(storage);
    editor.setAttribute(Qt::WA_DontShowOnScreen);
    editor.resize(600, 400);
    editor.show();
    panels.tree().setAttribute(Qt::WA_DontShowOnScreen);
    panels.tree().resize(300, 400);
    panels.tree().show();
    panels.listView().setAttribute(Qt::WA_DontShowOnScreen);
    panels.listView().resize(300, 400);
    panels.listView().show();

    // Проводка окна.
    QObject::connect(&editor, &zametti::NoteEditor::fileChanged, &panels, &NotePanels::setCurrentNote);
    QObject::connect(&editor, &zametti::NoteEditor::fileChanged, &panels,
                     [&panels](const QString& file) { panels.showNote(file); }, Qt::QueuedConnection);
    QObject::connect(&panels, &NotePanels::noteChosen, &editor,
                     [&editor](const QString& file, bool takeFocus) { editor.openFile(file, takeFocus); });
    QObject::connect(&editor, &zametti::NoteEditor::fileSaved, &panels,
                     [&panels](const QString& file) { panels.model().refreshNote(file); });

    zametti::NoteTreeModel& model = panels.model();
    zametti::NoteTreeView& tree = panels.tree();
    zametti::NoteListModel& list = panels.list();

    // Щелчок по папке А: первая заметка открыта, выделена, курсор на А.
    tree.setCurrentIndex(byTitle(model, QModelIndex(), QStringLiteral("А")));
    QCoreApplication::processEvents();
    QCoreApplication::processEvents();
    ZT_TRUE("открыта заметка из А", editor.filePath() == storage->pathOf(a1) || editor.filePath() == storage->pathOf(a2));
    ZT_EQ("панели знают открытую", s(editor.filePath()), s(panels.currentNote()));
    ZT_EQ("строка списка — открытая", s(editor.filePath()), s(list.pathAt(panels.listView().currentIndex())));
    ZT_EQ("курсор дерева на А", "А", s(model.titleOf(tree.currentIndex())));

    // Строку списка выбрали — открылась она.
    const QModelIndex other = list.indexForPath(editor.filePath() == storage->pathOf(a1) ? storage->pathOf(a2)
                                                                                         : storage->pathOf(a1));
    panels.listView().setCurrentIndex(other);
    QCoreApplication::processEvents();
    QCoreApplication::processEvents();
    ZT_EQ("выбор строки открыл её", s(list.pathAt(other)), s(editor.filePath()));

    // Правка и сохранение — строка списка обновилась (живой заголовок через каталог).
    editor.textCursor().insertText(QStringLiteral("Новое имя"));
    editor.save(false);
    QCoreApplication::processEvents();
    ZT_TRUE("список показал сохранённый заголовок",
            listTitles(list).join(QLatin1Char('|')).contains(QStringLiteral("Новое имя")));

    // Операция каталога (новая заметка в Б) — открытая на месте, курсор на А.
    const QString fresh = storage->createNote(b, false, &error);
    QCoreApplication::processEvents();
    ZT_EQ("операция каталога ничего не переоткрыла", s(list.pathAt(other)), s(editor.filePath()));
    ZT_EQ("курсор дерева всё на А", "А", s(model.titleOf(tree.currentIndex())));

    // Открыли заметку из Б откуда-то ещё (поиск): курсор остался на А, список — А;
    // Б получила вторичное выделение.
    const QStringList listA = listTitles(list);
    editor.openFile(storage->pathOf(b1));
    QCoreApplication::processEvents();
    QCoreApplication::processEvents();
    ZT_EQ("курсор дерева остался на А", "А", s(model.titleOf(tree.currentIndex())));
    ZT_EQ("список — по-прежнему А", s(listA.join(QLatin1Char('|'))), s(listTitles(list).join(QLatin1Char('|'))));
    ZT_EQ("вторичное — Б", s(storage->pathOf(b)), s(model.secondaryPath()));
    ZT_TRUE("свежая в списке А не появилась", !list.indexForPath(storage->pathOf(fresh)).isValid());

    // Старт: первичного выделения ещё нет — им становится папка открытой заметки.
    NotePanels fresh2(storage);
    fresh2.showNote(storage->pathOf(b1), /*primary=*/true);
    ZT_EQ("на старте курсор — папка открытой", "Б", s(fresh2.model().titleOf(fresh2.tree().currentIndex())));
    ZT_TRUE("и список — по ней", fresh2.list().indexForPath(storage->pathOf(b1)).isValid());

    return zt::report("панели с редактором");
}

TEST(NotePanels, All) {
    std::vector<QByteArray> ztArgs{QByteArrayLiteral("note_panels_test")};
    std::vector<char*> ztArgv;
    for (QByteArray& a : ztArgs) ztArgv.push_back(a.data());
    EXPECT_EQ(0, ztRunSuite(int(ztArgv.size()), ztArgv.data()));
    EXPECT_EQ(0, ztRunOwnerCopy());
    EXPECT_EQ(0, ztRunWired());
}
