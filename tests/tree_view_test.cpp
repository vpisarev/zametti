// Дерево папок ГЛАЗАМИ ВИДЖЕТА, а не модели.
//
// tree_test проверяет модель: что построилось, как отсортировалось, кого
// починили. Здесь другое — то, что видно только когда модель стоит в живом
// QTreeView и сигналы ходят по кругу «вид → модель → вид».
//
// Беда, ради которой набор и заведён (нашёл владелец). Закрыть папку A внутри
// папки B, закрыть B, открыть B — A правильно остаётся закрытой. Но следующий
// щелчок по A раскрывает её и ТУТ ЖЕ ЗАКРЫВАЕТ обратно; раскрывается она
// только со второго раза.

#include "note_tree.h"
#include "test_util.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>
#include <QTreeView>

#include <string>

namespace {

QString g_root;

void folder(const QString& id, const QString& title, const QString& parent) {
    QFile f(QDir(g_root).filePath(id + QStringLiteral(".md")));
    if (!f.open(QIODevice::WriteOnly)) return;
    QString text = QStringLiteral("<!-- zametti\nrole: folder\n");
    if (!parent.isEmpty()) text += QStringLiteral("parent: ") + parent + QLatin1Char('\n');
    text += QStringLiteral("-->\n\n# ") + title + QLatin1Char('\n');
    f.write(text.toUtf8());
}

QModelIndex findByTitle(const zametti::NoteTreeModel& model, const QModelIndex& parent,
                        const QString& title) {
    for (int row = 0; row < model.rowCount(parent); ++row) {
        const QModelIndex at = model.index(row, 0, parent);
        if (model.titleOf(at) == title) return at;
        const QModelIndex deeper = findByTitle(model, at, title);
        if (deeper.isValid()) return deeper;
    }
    return {};
}

}  // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QTemporaryDir tmp;
    if (!tmp.isValid()) {
        std::printf("не завёлся временный каталог\n");
        return 2;
    }
    g_root = tmp.path();
    QDir().mkpath(QDir(g_root).filePath(QStringLiteral(".zametti")));

    // B ⊃ A ⊃ C: у A обязан быть свой ребёнок, иначе раскрывать нечего.
    folder(QStringLiteral("01bbbbbbbbbbbb"), QStringLiteral("B"), QString());
    folder(QStringLiteral("01aaaaaaaaaaaa"), QStringLiteral("A"),
           QStringLiteral("01bbbbbbbbbbbb"));
    folder(QStringLiteral("01cccccccccccc"), QStringLiteral("C"),
           QStringLiteral("01aaaaaaaaaaaa"));

    zametti::NoteTreeModel model(g_root);

    // Дерево НАШЕ, а не голый QTreeView: у нашего нет треугольников ветвления,
    // и папку раскрывает обычный щелчок по строке. Вся беда именно в этом
    // переключении, и на голом QTreeView её не увидеть.
    zametti::NoteTreeView tree;
    tree.setModel(&model);
    // Ровно та же связка, что в окне: вид сообщает модели о раскрытии, модель
    // по этому признаку рисует значок папки.
    QObject::connect(&tree, &QTreeView::expanded, &tree,
                     [&model](const QModelIndex& i) { model.setExpanded(i, true); });
    QObject::connect(&tree, &QTreeView::collapsed, &tree,
                     [&model](const QModelIndex& i) { model.setExpanded(i, false); });
    tree.setAttribute(Qt::WA_DontShowOnScreen);
    tree.resize(300, 400);
    tree.show();

    const QModelIndex b = findByTitle(model, QModelIndex(), QStringLiteral("B"));
    ZT_TRUE("папка B нашлась", b.isValid());
    if (!b.isValid()) return zt::report("дерево в виджете");

    tree.expandAll();
    QCoreApplication::processEvents();
    const QModelIndex a = findByTitle(model, QModelIndex(), QStringLiteral("A"));
    ZT_TRUE("папка A нашлась", a.isValid());
    if (!a.isValid()) return zt::report("дерево в виджете");
    ZT_TRUE("после expandAll обе раскрыты", tree.isExpanded(a) && tree.isExpanded(b));

    // Ровно последовательность владельца.
    tree.collapse(a);
    tree.collapse(b);
    QCoreApplication::processEvents();
    tree.expand(b);
    QCoreApplication::processEvents();
    ZT_TRUE("B раскрылась", tree.isExpanded(b));
    ZT_TRUE("а A осталась закрытой, как и было", !tree.isExpanded(a));

    // И вот теперь — то самое раскрытие A.
    tree.expand(a);
    ZT_TRUE("A раскрылась сразу", tree.isExpanded(a));
    QCoreApplication::processEvents();
    ZT_TRUE("и осталась раскрытой, а не захлопнулась", tree.isExpanded(a));

    // Про щелчок здесь проверки НЕТ, и это не забывчивость. Живой стиль
    // рабочего стола переключает раскрытость папки сам, на отпускании мыши,
    // сигналом clicked — это и видно в стеке под Xvfb. А под offscreen-стилем
    // набора тот же щелчок не переключает ничего: свойство принадлежит стилю,
    // и проверка на него краснела бы там, где всё в порядке.
    //
    // Поэтому проверяется не чужое поведение, а НАШЕ правило: показ строки не
    // трогает её собственную раскрытость. Оно верно при любом стиле, и именно
    // его нарушение давало беду.

    // --- ПОКАЗ СТРОКИ НЕ ТРОГАЕТ ЕЁ СОБСТВЕННУЮ РАСКРЫТОСТЬ ---------------
    //
    // Ради этого правила и заведена expandAncestors. Раскрой она заодно и саму
    // папку — на один щелчок пришлось бы два переключения (наше и Qt), и папка
    // захлопывалась бы сразу после открытия. Владелец видел ровно это.
    tree.collapse(a);
    tree.collapse(b);
    QCoreApplication::processEvents();
    ZT_TRUE("B закрыта, A внутри неё закрыта", !tree.isExpanded(b) && !tree.isExpanded(a));
    zametti::expandAncestors(tree, a);
    QCoreApplication::processEvents();
    ZT_TRUE("показ строки A раскрыл её родителя", tree.isExpanded(b));
    ZT_TRUE("а саму A не тронул", !tree.isExpanded(a));

    // --- ТА САМАЯ БЕДА ----------------------------------------------------
    //
    // Щелчок приходит на ОТПУСКАНИИ. Между нажатием и отпусканием окно успевает
    // показать в дереве открытую заметку и по дороге раскрыть папку — и если
    // спросить раскрытость в этот момент, переключатель увидит «уже открыта» и
    // закроет. На один щелчок два переключения.
    //
    // Здесь это воспроизводится дословно: нажали, раскрыли чужой рукой,
    // отпустили. Ждём, что папка ОСТАНЕТСЯ раскрытой: щелчок значит ровно то,
    // что человеку было видно, когда он нажимал.
    tree.collapse(a);
    tree.expand(b);
    QCoreApplication::processEvents();
    const QRect row = tree.visualRect(a);
    ZT_TRUE("строка A видна", !row.isEmpty());
    QTest::mousePress(tree.viewport(), Qt::LeftButton, Qt::NoModifier, row.center());
    tree.expand(a);   // так делает показ открытой заметки
    QCoreApplication::processEvents();
    QTest::mouseRelease(tree.viewport(), Qt::LeftButton, Qt::NoModifier, row.center());
    QCoreApplication::processEvents();
    ZT_TRUE("папка не захлопнулась от собственного щелчка", tree.isExpanded(a));

    // А обычный щелчок по раскрытой папке её всё так же закрывает: переключение
    // никуда не делось, оно лишь считается от состояния на нажатии.
    QTest::mouseClick(tree.viewport(), Qt::LeftButton, Qt::NoModifier, row.center());
    QCoreApplication::processEvents();
    ZT_TRUE("а повторный щелчок закрывает", !tree.isExpanded(a));

    return zt::report("дерево в виджете");
}
