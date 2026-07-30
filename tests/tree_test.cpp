// Дерево плоского хранилища: строится из метаданных, чинится в памяти и
// никогда не пишет в файлы. По списку брифа: сирота, цикл, пустая заметка,
// заметка без заголовка, сортировка по modified со свежими сверху, корзина
// в самом низу корня, запрет переноса в собственное поддерево.

#include "note_tree.h"

#include "test_util.h"

#include <QApplication>
#include <QDir>
#include <QFile>

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

    ZT_TRUE("хранилище распознано", NoteTreeModel::isStoreRoot(g_root));
    NoteTreeModel model(g_root);
    ZT_TRUE("режим хранилища", model.isStore());

    const int rootRows = model.rowCount(QModelIndex());
    // Корень: папка, сирота, виновник цикла (заложник остаётся его ребёнком —
    // рвётся одно ребро, а не всё), пустая, без заголовка, корзина = 6.
    ZT_TRUE("в корне шесть узлов", rootRows == 6);

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
    ZT_TRUE("папка найдена и это заметка",
            folder.isValid() && !model.filePath(folder).isEmpty());
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

    QDir(g_root).removeRecursively();
    return zt::report("дерево хранилища");
}
