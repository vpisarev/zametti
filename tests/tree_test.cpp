// Дерево плоского хранилища: строится из метаданных, чинится в памяти и
// никогда не пишет в файлы. По списку брифа: сирота, цикл, пустая заметка,
// заметка без заголовка, сортировка по modified со свежими сверху, корзина
// в самом низу корня, запрет переноса в собственное поддерево.

#include "note_tree.h"

#include "icons.h"
#include "settings.h"
#include "test_util.h"

#include <vector>
#include "testdata.h"

#include <unistd.h>
#include <cstdio>

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFontMetrics>
#include <QMimeData>
#include <QPixmap>

#include <memory>

#include <string>

using zametti::NoteTreeModel;

namespace {

QString g_root;

void note(const QString& id, const QString& meta, const QString& body) {
    QFile f(g_root + QLatin1Char('/') + id + QStringLiteral(".md"));
    if (!f.open(QIODevice::WriteOnly)) return;
    QString text = QStringLiteral("<!-- zametti\n") + meta +
                   QStringLiteral("-->\n");
    if (!body.isEmpty()) text += QStringLiteral("\n") + body;
    f.write(text.toUtf8());
}

QString titleAt(const NoteTreeModel& model, const QModelIndex& parent, int row) {
    return model.data(model.index(row, 0, parent), Qt::DisplayRole).toString();
}

// Каким значком нарисована строка. Размер и цвет считаются ровно так же, как в
// note_tree.cpp, а сравнение идёт по cacheKey: кэш иконок на одинаковый запрос
// отдаёт ОДИН И ТОТ ЖЕ растр, и равенство ключей означает «нарисована именно
// эта иконка», а не «похожа на неё». Сравнивать пиксели тут нельзя — folder и
// folder-open различаются десятком точек, и порог «различаются» прошёл бы и на
// сглаживании.
bool iconIs(const NoteTreeModel& model, const QModelIndex& index, const char* name) {
    const zametti::ZSettings::Ui& a = zametti::settings().ui();
    QFont font;
    font.setPointSizeF(a.sidebarFontPoint() * a.sidebarFolderScale());
    const int side = QFontMetrics(font).height();
    const qreal dpr = qGuiApp != nullptr ? qGuiApp->devicePixelRatio() : 1.0;
    const QPixmap want =
        zametti::toolbarIcon(QString::fromLatin1(name), side, a.sidebarFolderColor(), dpr);
    const QVariant got = model.data(index, Qt::DecorationRole);
    if (want.isNull() || !got.canConvert<QPixmap>()) return false;
    return got.value<QPixmap>().cacheKey() == want.cacheKey();
}

}  // namespace

static int ztRunSuite(int argc, char** argv) {
    (void)argc;
    (void)argv;
    g_root = QDir::tempPath() + QStringLiteral("/zametti-tree-test");
    QDir(g_root).removeRecursively();
    QDir().mkpath(g_root + QStringLiteral("/.zametti"));

    // Папка (заметка с детьми), два ребёнка с разным modified, сирота, цикл
    // из двух заметок, пустая, без заголовка, корзина.
    note("00000000000001", "created: 2020-01-01T00:00:00Z\nmodified: 2020-05-01T00:00:00Z\n",
         "# Папка\n");
    note("00000000000002",
         "parent: 00000000000001\ncreated: 2020-01-01T00:00:00Z\n"
         "modified: 2021-01-01T00:00:00Z\n",
         "# Старый ребёнок\n");
    note("00000000000003",
         "parent: 00000000000001\ncreated: 2020-01-01T00:00:00Z\n"
         "modified: 2024-01-01T00:00:00Z\n",
         "# Свежий ребёнок\n");
    note("00000000000004", "parent: 000000000000zz\nmodified: 2022-01-01T00:00:00Z\n",
         "# Сирота\n");
    note("00000000000005", "parent: 00000000000006\nmodified: 2023-06-01T00:00:00Z\n",
         "# Цикл А\n");
    note("00000000000006", "parent: 00000000000005\nmodified: 2023-06-02T00:00:00Z\n",
         "# Цикл Б\n");
    note("00000000000007", "modified: 2019-01-01T00:00:00Z\n", "");
    note("00000000000008", "modified: 2025-01-01T00:00:00Z\n",
         "просто первая строка текста, длинная и без всякого заголовка\n");
    note("0000000000000t", "role: trash\nmodified: 2026-01-01T00:00:00Z\n",
         "# Корзина\n");
    note("0000000000000v",
         "parent: 0000000000000t\nmodified: 2020-02-02T00:00:00Z\n",
         "# Выброшенная\n");
    note("0000000000000f", "role: folder\nmodified: 2018-01-01T00:00:00Z\n",
         "# Пустая папка\n");

    // Нечитаемая заметка. Пропасть из дерева она может — прочесть её нечем, —
    // но пропасть МОЛЧА не имеет права: файл на месте, человек его видит, а
    // программа делает вид, что заметки нет. Ловим саму жалобу: перехватываем
    // stderr и смотрим, назван ли в нём путь.
    note("0000000000000x", "modified: 2026-02-02T00:00:00Z\n", "# Нечитаемая\n");
    const QString locked = g_root + QStringLiteral("/0000000000000x.md");
    const bool hidden = QFile::setPermissions(locked, QFile::Permissions());
    const QString complaints = QDir::tempPath() + QStringLiteral("/zametti-tree-stderr.txt");
    QFile::remove(complaints);
    if (hidden) {
        std::fflush(stderr);
        const int saved = dup(fileno(stderr));
        FILE* redirected = std::freopen(complaints.toUtf8().constData(), "w", stderr);
        NoteTreeModel quiet(g_root);
        (void)quiet.rowCount(QModelIndex());
        std::fflush(stderr);
        if (redirected != nullptr) {
            dup2(saved, fileno(stderr));
            ::close(saved);
        }
        QFile file(complaints);
        QString said;
        if (file.open(QIODevice::ReadOnly)) said = QString::fromUtf8(file.readAll());
        ZT_TRUE("о нечитаемой заметке сказано вслух",
                said.contains(QStringLiteral("0000000000000x")));
        QFile::setPermissions(locked, QFile::ReadOwner | QFile::WriteOwner);
    }
    QFile::remove(locked);

    ZT_TRUE("хранилище распознано", NoteTreeModel::isStoreRoot(g_root));
    NoteTreeModel model(g_root);
    ZT_TRUE("режим хранилища", model.isStore());

    // Первая и единственная строка верхнего уровня — «All notes» (этап 4):
    // корень хранилища виден всегда, содержимое лежит под ним.
    ZT_TRUE("верхний уровень — одна строка «все заметки»",
            model.rowCount(QModelIndex()) == 1);
    // Не const: смена сортировки пересобирает модель, и все прежние индексы
    // становятся недействительными — их надо брать заново (на этом пойман
    // сегфолт при первом прогоне).
    QModelIndex all = model.index(0, 0, QModelIndex());
    ZT_TRUE("корневая строка — директория", model.isDirectory(all));
    ZT_TRUE("корневая строка не открывается", model.filePath(all).isEmpty());

    const int rootRows = model.rowCount(all);
    // Под корнем: папка, сирота, виновник цикла (заложник остаётся его ребёнком —
    // рвётся одно ребро, а не всё), пустая, без заголовка, пустая папка,
    // корзина = 7.
    ZT_TRUE("в корне семь узлов", rootRows == 7);

    // Живой каталог всплывает: у «Папки» ребёнок правлен в 2024 — она выше
    // сироты 2022 года, хотя своя правка 2020-го.
    ZT_TRUE("каталог с недавней правкой внутри — впереди",
            titleAt(model, all, 1) == QStringLiteral("Папка"));

    // Пустая папка — директория по мете: не открывается, со значком.
    {
        const QModelIndex folder = model.indexForPath(
            g_root + QStringLiteral("/0000000000000f.md"));
        ZT_TRUE("пустая папка — директория", model.isDirectory(folder));
        ZT_TRUE("директория не открывается", model.filePath(folder).isEmpty());
    }

    // Сортировка по имени: директории первыми, Архив всё равно внизу.
    model.setRootSort(zametti::defaultOrder(zametti::SortKey::Name));
    all = model.index(0, 0, QModelIndex());
    ZT_TRUE("по имени: первая — директория",
            model.isDirectory(model.index(0, 0, all)));
    ZT_TRUE("по имени: Архив внизу",
            titleAt(model, all, model.rowCount(all) - 1) == QStringLiteral("Archive"));
    model.setRootSort(zametti::defaultOrder(zametti::SortKey::Modified));
    all = model.index(0, 0, QModelIndex());

    // Свежие сверху, Архив — последним, несмотря на свежий modified внутри.
    ZT_TRUE("первый — без заголовка (2025)",
            titleAt(model, all, 0).startsWith(QStringLiteral("просто первая строка")));
    ZT_TRUE("Архив в самом низу",
            titleAt(model, all, rootRows - 1) == QStringLiteral("Archive"));
    ZT_TRUE("пустая — «Без названия»",
            [&] {
                for (int i = 0; i < rootRows; ++i)
                    if (titleAt(model, all, i) == QStringLiteral("Untitled")) return true;
                return false;
            }());

    // Сирота и цикл — в корне с пометками.
    int badges = 0;
    bool orphan = false;
    bool cycle = false;
    for (int i = 0; i < rootRows; ++i) {
        const QString t = titleAt(model, all, i);
        if (t.contains(QStringLiteral("[orphan]"))) { orphan = true; ++badges; }
        if (t.contains(QStringLiteral("[cycle]"))) { cycle = true; ++badges; }
    }
    ZT_TRUE("сирота помечена", orphan);
    ZT_TRUE("цикл разорван и помечен", cycle);
    ZT_TRUE("пометок ровно две", badges == 2);
    // Заложник цикла висит под виновником, а не потерян.
    {
        const QModelIndex culpritA = model.indexForPath(
            g_root + QStringLiteral("/00000000000005.md"));
        const QModelIndex culpritB = model.indexForPath(
            g_root + QStringLiteral("/00000000000006.md"));
        ZT_TRUE("обе заметки цикла в дереве", culpritA.isValid() && culpritB.isValid());
        ZT_TRUE("одна из них — ребёнок другой",
                culpritA.parent() == culpritB || culpritB.parent() == culpritA);
    }

    // Дети папки: свежий выше старого; сама папка открывается как заметка.
    const QModelIndex folder = model.indexForPath(
        g_root + QStringLiteral("/00000000000001.md"));
    ZT_TRUE("папка найдена и это директория: не открывается",
            folder.isValid() && model.isDirectory(folder) &&
                model.filePath(folder).isEmpty());
    ZT_TRUE("у папки двое детей", model.rowCount(folder) == 2);
    ZT_TRUE("свежий ребёнок выше",
            titleAt(model, folder, 0) == QStringLiteral("Свежий ребёнок"));

    // АРХИВ И ЕГО СОДЕРЖИМОЕ. Старая заметка-корзина (`role: trash`) читается
    // как архивная — псевдоним на чтении для хранилищ, не прошедших разовую
    // миграцию, — и вместе со своим содержимым лежит в виртуальном «Архиве».
    ZT_TRUE("в Архиве найдены и корзина, и выброшенная",
            model.archivedIds().contains(QStringLiteral("0000000000000t")) &&
                model.archivedIds().contains(QStringLiteral("0000000000000v")));
    const QModelIndex thrown = model.indexForPath(
        g_root + QStringLiteral("/0000000000000v.md"));
    ZT_TRUE("выброшенная лежит в Архиве", model.inArchive(thrown));
    ZT_TRUE("папка не в Архиве", !model.inArchive(folder));

    // ПУСТОЙ АРХИВ ВСЁ РАВНО ВИДЕН. Он собирается из помеченных заметок, и
    // раньше исчезал вместе с последней: владелец вернул всё из архива — и
    // место, куда он привык убирать, пропало из дерева. Прежняя Корзина была
    // настоящей папкой и стояла всегда; Архив обязан вести себя так же.
    {
        const QString root = g_root + QStringLiteral("/пустой-архив");
        QDir().mkpath(root + QStringLiteral("/.zametti"));
        QFile note(root + QStringLiteral("/0000000000000a.md"));
        if (note.open(QIODevice::WriteOnly))
            note.write("<!-- zametti\ncreated: 2026-01-01T00:00:00Z\n"
                       "modified: 2026-01-01T00:00:00Z\n-->\n\n# Одна заметка\n");
        note.close();
        NoteTreeModel empty(root);
        bool seen = false;
        const QModelIndex inside = empty.index(0, 0, QModelIndex());
        for (int row = 0; row < empty.rowCount(inside); ++row)
            if (titleAt(empty, inside, row) == QStringLiteral("Archive")) seen = true;
        ZT_TRUE("в хранилище без архивных заметок Архив всё равно есть", seen);
    }

    // Запрет переноса в собственное поддерево.
    ZT_TRUE("ребёнок — потомок папки",
            model.isDescendantOf(QStringLiteral("00000000000002"),
                                 QStringLiteral("00000000000001")));
    ZT_TRUE("папка не потомок ребёнка",
            !model.isDescendantOf(QStringLiteral("00000000000001"),
                                  QStringLiteral("00000000000002")));

    // Роли неизменны: заметка никогда не становится папкой, папка —
    // заметкой. Создание целится в ближайшую ПАПКУ, сброс на заметку
    // запрещён на уровне модели.
    {
        const QModelIndex noteInFolder = model.indexForPath(
            g_root + QStringLiteral("/00000000000002.md"));
        // Ближайшая папка: для заметки — её родитель, для папки — она сама,
        // для корня — пусто.
        ZT_TRUE("создание от заметки целится в её папку",
                model.folderIdFor(noteInFolder) == QStringLiteral("00000000000001"));
        ZT_TRUE("создание от папки целится в неё саму",
                model.folderIdFor(folder) == QStringLiteral("00000000000001"));
        ZT_TRUE("создание без выбора — в корень",
                model.folderIdFor(QModelIndex()).isEmpty());

        // Матрица сброса: на заметку нельзя, в папку и корень можно, папку в
        // папку можно, в своё поддерево нельзя.
        const QModelIndex emptyFolder = model.indexForPath(
            g_root + QStringLiteral("/0000000000000f.md"));
        const QModelIndex orphan = model.indexForPath(
            g_root + QStringLiteral("/00000000000004.md"));
        std::shared_ptr<QMimeData> dragNote(model.mimeData({orphan}));
        ZT_TRUE("сброс заметки НА ЗАМЕТКУ запрещён",
                !model.canDropMimeData(dragNote.get(), Qt::MoveAction, -1, -1,
                                       noteInFolder));
        ZT_TRUE("сброс заметки в папку разрешён",
                model.canDropMimeData(dragNote.get(), Qt::MoveAction, -1, -1,
                                      emptyFolder));
        ZT_TRUE("сброс заметки в корень разрешён",
                model.canDropMimeData(dragNote.get(), Qt::MoveAction, -1, -1,
                                      QModelIndex()));
        std::shared_ptr<QMimeData> dragFolder(model.mimeData({folder}));
        ZT_TRUE("папку в папку можно",
                model.canDropMimeData(dragFolder.get(), Qt::MoveAction, -1, -1,
                                      emptyFolder));
        ZT_TRUE("папку в её же заметку нельзя",
                !model.canDropMimeData(dragFolder.get(), Qt::MoveAction, -1, -1,
                                       noteInFolder));
        std::shared_ptr<QMimeData> dragEmpty(model.mimeData({emptyFolder}));
        ZT_TRUE("пустую папку в другую папку можно",
                model.canDropMimeData(dragEmpty.get(), Qt::MoveAction, -1, -1, folder));
    }

    // Никакая работа модели не трогает файлы: байты до и после всех
    // манипуляций (обновления подписи, смены сортировок, перечитывания)
    // совпадают — роли в том числе.
    QStringList before;
    for (const QFileInfo& info :
         QDir(g_root).entryInfoList({QStringLiteral("*.md")}, QDir::Files)) {
        QFile f(info.filePath());
        if (f.open(QIODevice::ReadOnly)) before.append(QString::fromUtf8(f.readAll()));
    }

    // Живой заголовок.
    model.updateTitle(g_root + QStringLiteral("/00000000000003.md"),
                      QStringLiteral("Совсем свежий"));
    ZT_TRUE("подпись обновилась",
            titleAt(model, folder, 0) == QStringLiteral("Совсем свежий"));

    // Загрузчик не пишет: байты всех файлов не тронуты.
    {
        QFile f(g_root + QStringLiteral("/00000000000004.md"));
        ZT_TRUE("файл открылся", f.open(QIODevice::ReadOnly));
        ZT_TRUE("файл сироты не тронут",
                QString::fromUtf8(f.readAll())
                    .contains(QStringLiteral("parent: 000000000000zz")));
    }

    model.setRootSort(zametti::defaultOrder(zametti::SortKey::Name));
    model.setRootSort(zametti::defaultOrder(zametti::SortKey::Modified));
    model.refresh();
    QStringList after;
    for (const QFileInfo& info :
         QDir(g_root).entryInfoList({QStringLiteral("*.md")}, QDir::Files)) {
        QFile f(info.filePath());
        if (f.open(QIODevice::ReadOnly)) after.append(QString::fromUtf8(f.readAll()));
    }
    ZT_TRUE("модель не изменила ни байта ни в одном файле", before == after);

    // --- средняя колонка: плоский список поддерева (этап 4) ----------------
    //
    // Вложенность на два уровня: заметка из под-под-папки обязана быть видна в
    // списке корня — в этом весь смысл плоскости.
    note("0000000000000a", "role: folder\nparent: 00000000000001\n"
                           "modified: 2024-02-02T00:00:00Z\n",
         "# Подпапка\n");
    note("0000000000000b",
         "parent: 0000000000000a\nmodified: 2024-03-03T00:00:00Z\n",
         "# Глубокая\n\nПервый **жирный** абзац с `кодом`.\n\nВторой абзац.\n");
    model.refresh();
    all = model.index(0, 0, QModelIndex());

    {
        const std::vector<zametti::NoteRow> rows = model.notesInSubtree(all);
        const auto has = [&rows](const QString& title) {
            for (const auto& r : rows)
                if (r.title == title) return true;
            return false;
        };
        ZT_TRUE("заметка из под-под-папки видна в списке корня", has(QStringLiteral("Глубокая")));
        ZT_TRUE("папки в списке не показываются",
                !has(QStringLiteral("Подпапка")) && !has(QStringLiteral("Папка")) &&
                    !has(QStringLiteral("Пустая папка")));
        ZT_TRUE("корзина в общий список не попадает",
                !has(QStringLiteral("Корзина")) && !has(QStringLiteral("Выброшенная")));

        // Сниппет — текст блоков после заголовка, без маркеров разметки и без
        // метаданных.
        QString snippet;
        for (const auto& r : rows)
            if (r.title == QStringLiteral("Глубокая")) snippet = r.snippet;
        ZT_TRUE("сниппет взят из тела", snippet.startsWith(QStringLiteral("Первый жирный")));
        ZT_TRUE("в сниппете нет маркеров разметки",
                !snippet.contains(QLatin1Char('*')) && !snippet.contains(QLatin1Char('`')));
        ZT_TRUE("в сниппете нет метаданных",
                !snippet.contains(QStringLiteral("modified")) &&
                    !snippet.contains(QStringLiteral("parent")));
        ZT_TRUE("сниппет продолжается вторым абзацем",
                snippet.contains(QStringLiteral("Второй абзац")));
        ZT_TRUE("заголовок в сниппет не попал",
                !snippet.contains(QStringLiteral("Глубокая")));
    }

    // Список папки — только её поддерево; изнутри корзины видно выброшенное.
    {
        const QModelIndex sub = model.indexForPath(
            g_root + QStringLiteral("/0000000000000a.md"));
        const std::vector<zametti::NoteRow> rows = model.notesInSubtree(sub);
        ZT_TRUE("в подпапке ровно одна заметка", rows.size() == 1);
        ZT_TRUE("и это она", rows.empty() || rows[0].title == QStringLiteral("Глубокая"));

        const QModelIndex trash = model.indexForPath(
            g_root + QStringLiteral("/0000000000000t.md"));
        const std::vector<zametti::NoteRow> thrown = model.notesInSubtree(trash);
        ZT_TRUE("внутри корзины её содержимое видно",
                thrown.size() == 1 && thrown[0].title == QStringLiteral("Выброшенная"));
    }

    // Режим «только папки»: заметок в модели нет, папки на месте, а список
    // средней колонки от этого не меняется — он берёт данные из дерева целиком.
    model.setFoldersOnly(true);
    all = model.index(0, 0, QModelIndex());
    {
        bool onlyDirs = true;
        for (int row = 0; row < model.rowCount(all); ++row)
            if (!model.isDirectory(model.index(row, 0, all))) onlyDirs = false;
        ZT_TRUE("в левой панели остались только папки", onlyDirs);
        ZT_TRUE("папка на месте",
                model.indexForPath(g_root + QStringLiteral("/00000000000001.md")).isValid());
        ZT_TRUE("заметка из левой панели пропала",
                !model.indexForPath(g_root + QStringLiteral("/00000000000003.md")).isValid());
        ZT_TRUE("но списку она видна",
                model.notesInSubtree(QModelIndex()).size() ==
                    model.notesInSubtree(all).size());
        ZT_TRUE("и по id она находится",
                model.hasNote(QStringLiteral("00000000000003")));

        // Значок папки в этом режиме. «Папка» содержит «Подпапку» — её есть
        // куда раскрывать, значок закрытый. А вот «Подпапка» держит только
        // заметки: строк под ней ноль, щёлкай сколько хочешь. Она рисовалась
        // закрытой, потому что значок выбирался по children (заметки там есть),
        // а не по shown (видимых строк нет).
        const QModelIndex folder =
            model.indexForPath(g_root + QStringLiteral("/00000000000001.md"));
        const QModelIndex leaf =
            model.indexForPath(g_root + QStringLiteral("/0000000000000a.md"));
        ZT_TRUE("папка с подпапкой, пока не раскрыта, — значок закрытой",
                iconIs(model, folder, "folder"));
        ZT_TRUE("папка без подпапок — значок открытой, раскрывать нечего",
                iconIs(model, leaf, "folder-open"));
        model.setExpanded(folder, true);
        ZT_TRUE("раскрытая — значок открытой", iconIs(model, folder, "folder-open"));
        model.setExpanded(folder, false);
    }

    // Запросы по id: ими живут операции над заметками, пока индексов у них нет.
    ZT_TRUE("родитель по id",
            model.parentIdOf(QStringLiteral("0000000000000b")) ==
                QStringLiteral("0000000000000a"));
    ZT_TRUE("путь из имён папок",
            model.ancestorTitles(QStringLiteral("0000000000000b"))
                    .join(QLatin1Char('/')) == QStringLiteral("Папка/Подпапка"));
    ZT_TRUE("папка по имени внутри папки",
            model.childFolderByTitle(QStringLiteral("00000000000001"),
                                     QStringLiteral("Подпапка")) ==
                QStringLiteral("0000000000000a"));
    ZT_TRUE("папка не считается заметкой",
            model.isFolderId(QStringLiteral("0000000000000a")) &&
                !model.isFolderId(QStringLiteral("0000000000000b")));
    ZT_TRUE("архивность видна по id",
            model.inArchiveId(QStringLiteral("0000000000000v")) &&
                !model.inArchiveId(QStringLiteral("0000000000000b")));
    ZT_TRUE("первая открываемая заметка — не папка",
            !model.firstNoteId().isEmpty() && !model.isFolderId(model.firstNoteId()));

    // Шапку заметки может испортить чужой редактор: «-->» внутри значения
    // закрывает комментарий раньше времени, и остаток шапки становится телом.
    // Требование этапа 4: одна такая заметка не ломает разбор соседних.
    {
        note("0000000000000c",
             "parent: 00000000000001\nmodified: 2024-04-04T00:00:00Z\n"
             "trash-path: папка --> другая\nrole: folder\n",
             "# Битая\n");
        model.refresh();
        all = model.index(0, 0, QModelIndex());

        ZT_TRUE("заметка с битой шапкой в дереве есть",
                model.hasNote(QStringLiteral("0000000000000c")));
        // Ключи после разрыва не читаются — это следствие самого формата, а не
        // наша беда: role: folder оказался уже в теле.
        ZT_TRUE("ключи после разрыва в мету не попали",
                !model.isFolderId(QStringLiteral("0000000000000c")));
        // А главное — соседи целы: и структура, и заголовки, и список.
        ZT_TRUE("сосед по папке не пострадал",
                model.parentIdOf(QStringLiteral("0000000000000b")) ==
                    QStringLiteral("0000000000000a"));
        ZT_TRUE("заголовки соседей на месте",
                model.titleOfId(QStringLiteral("0000000000000b")) ==
                    QStringLiteral("Глубокая"));
        bool deepStillListed = false;
        for (const auto& row : model.notesInSubtree(all))
            if (row.title == QStringLiteral("Глубокая")) deepStillListed = true;
        ZT_TRUE("список заметок собрался целиком", deepStillListed);
    }

    // Заголовок для окна берётся по id, а не по QModelIndex. Дерево показывает
    // только папки, поэтому видимой строки у заметки нет вовсе, и indexForPath
    // для неё пуст — кто спросит заголовок через индекс, покажет человеку id.
    // Ровно на этом заголовок окна показывал имя файла вместо названия заметки.
    {
        const QString noteFile =
            g_root + QStringLiteral("/0000000000000b.md");
        const QString folderFile =
            g_root + QStringLiteral("/0000000000000a.md");
        model.setFoldersOnly(true);
        ZT_TRUE("у заметки видимой строки в дереве нет",
                !model.indexForPath(noteFile).isValid());
        ZT_TRUE("а у папки есть", model.indexForPath(folderFile).isValid());
        ZT_TRUE("заголовок заметки по id находится",
                model.titleOfId(QStringLiteral("0000000000000b")) ==
                    QStringLiteral("Глубокая"));
        ZT_TRUE("и заголовок папки по id тоже",
                !model.titleOfId(QStringLiteral("0000000000000a")).isEmpty());
    }

    // Строка списка одной заметки — та же, что в общем списке.
    {
        const zametti::NoteRow row = model.rowOf(QStringLiteral("0000000000000b"));
        ZT_TRUE("строка по id заполнена",
                row.title == QStringLiteral("Глубокая") && !row.snippet.isEmpty() &&
                    row.modified == QStringLiteral("2024-03-03T00:00:00Z"));
    }

    QDir(g_root).removeRecursively();
    return zt::report("дерево хранилища");
}

// Набор целиком одним TEST: тело не тронуто, argv ему собран здесь.
// Дробить на отдельные проверки — отдельная работа, по одному набору.
TEST(Tree, All) {
    std::vector<QByteArray> ztArgs{QByteArrayLiteral("tree_test")};
    std::vector<char*> ztArgv;
    for (QByteArray& a : ztArgs) ztArgv.push_back(a.data());
    EXPECT_EQ(0, ztRunSuite(int(ztArgv.size()), ztArgv.data()));
}
