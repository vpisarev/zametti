// Дерево плоского хранилища: строится из метаданных, чинится в памяти и
// никогда не пишет в файлы. По списку брифа: сирота, цикл, пустая заметка,
// заметка без заголовка, сортировка по modified со свежими сверху, корзина
// в самом низу корня, запрет переноса в собственное поддерево.

#include "note_tree.h"

#include "test_util.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMimeData>

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

}  // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
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

    ZT_TRUE("хранилище распознано", NoteTreeModel::isStoreRoot(g_root));
    NoteTreeModel model(g_root);
    ZT_TRUE("режим хранилища", model.isStore());

    const int rootRows = model.rowCount(QModelIndex());
    // Корень: папка, сирота, виновник цикла (заложник остаётся его ребёнком —
    // рвётся одно ребро, а не всё), пустая, без заголовка, пустая папка,
    // корзина = 7.
    ZT_TRUE("в корне семь узлов", rootRows == 7);

    // Живой каталог всплывает: у «Папки» ребёнок правлен в 2024 — она выше
    // сироты 2022 года, хотя своя правка 2020-го.
    ZT_TRUE("каталог с недавней правкой внутри — впереди",
            titleAt(model, {}, 1) == QStringLiteral("Папка"));

    // Пустая папка — директория по мете: не открывается, со значком.
    {
        const QModelIndex folder = model.indexForPath(
            g_root + QStringLiteral("/0000000000000f.md"));
        ZT_TRUE("пустая папка — директория", model.isDirectory(folder));
        ZT_TRUE("директория не открывается", model.filePath(folder).isEmpty());
    }

    // Сортировка по имени: директории первыми, корзина всё равно внизу.
    model.setSortMode(NoteTreeModel::SortMode::ByName);
    ZT_TRUE("по имени: первая — директория",
            model.isDirectory(model.index(0, 0, QModelIndex())));
    ZT_TRUE("по имени: корзина внизу",
            titleAt(model, {}, model.rowCount(QModelIndex()) - 1) ==
                QStringLiteral("Корзина"));
    model.setSortMode(NoteTreeModel::SortMode::ByModified);

    // Свежие сверху, корзина — последней, несмотря на свежий modified.
    ZT_TRUE("первый — без заголовка (2025)",
            titleAt(model, {}, 0).startsWith(QStringLiteral("просто первая строка")));
    ZT_TRUE("корзина в самом низу",
            titleAt(model, {}, rootRows - 1) == QStringLiteral("Корзина"));
    ZT_TRUE("пустая — «Без названия»",
            [&] {
                for (int i = 0; i < rootRows; ++i)
                    if (titleAt(model, {}, i) == QStringLiteral("Без названия")) return true;
                return false;
            }());

    // Сирота и цикл — в корне с пометками.
    int badges = 0;
    bool orphan = false;
    bool cycle = false;
    for (int i = 0; i < rootRows; ++i) {
        const QString t = titleAt(model, {}, i);
        if (t.contains(QStringLiteral("[сирота]"))) { orphan = true; ++badges; }
        if (t.contains(QStringLiteral("[цикл]"))) { cycle = true; ++badges; }
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

    // Корзина и её содержимое.
    ZT_TRUE("id корзины найден", model.trashId() == QStringLiteral("0000000000000t"));
    const QModelIndex thrown = model.indexForPath(
        g_root + QStringLiteral("/0000000000000v.md"));
    ZT_TRUE("выброшенная лежит в корзине", model.inTrash(thrown));
    ZT_TRUE("папка не в корзине", !model.inTrash(folder));

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
        std::unique_ptr<QMimeData> dragNote(model.mimeData({orphan}));
        ZT_TRUE("сброс заметки НА ЗАМЕТКУ запрещён",
                !model.canDropMimeData(dragNote.get(), Qt::MoveAction, -1, -1,
                                       noteInFolder));
        ZT_TRUE("сброс заметки в папку разрешён",
                model.canDropMimeData(dragNote.get(), Qt::MoveAction, -1, -1,
                                      emptyFolder));
        ZT_TRUE("сброс заметки в корень разрешён",
                model.canDropMimeData(dragNote.get(), Qt::MoveAction, -1, -1,
                                      QModelIndex()));
        std::unique_ptr<QMimeData> dragFolder(model.mimeData({folder}));
        ZT_TRUE("папку в папку можно",
                model.canDropMimeData(dragFolder.get(), Qt::MoveAction, -1, -1,
                                      emptyFolder));
        ZT_TRUE("папку в её же заметку нельзя",
                !model.canDropMimeData(dragFolder.get(), Qt::MoveAction, -1, -1,
                                       noteInFolder));
        std::unique_ptr<QMimeData> dragEmpty(model.mimeData({emptyFolder}));
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

    model.setSortMode(NoteTreeModel::SortMode::ByName);
    model.setSortMode(NoteTreeModel::SortMode::ByModified);
    model.refresh();
    QStringList after;
    for (const QFileInfo& info :
         QDir(g_root).entryInfoList({QStringLiteral("*.md")}, QDir::Files)) {
        QFile f(info.filePath());
        if (f.open(QIODevice::ReadOnly)) after.append(QString::fromUtf8(f.readAll()));
    }
    ZT_TRUE("модель не изменила ни байта ни в одном файле", before == after);

    QDir(g_root).removeRecursively();
    return zt::report("дерево хранилища");
}
