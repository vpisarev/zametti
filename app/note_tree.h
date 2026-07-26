// Дерево заметок для боковой панели.
//
// Модель своя, а не QFileSystemModel. У того две беды для нашего случая: он
// асинхронный (каталог догружается уже после того, как мы попросили выделить
// текущую заметку) и показывает каталоги, в которых заметок нет вовсе. Здесь
// дерево строится один раз обходом каталога — для сотен файлов это мгновенно, —
// пустые ветки отбрасываются, а в подписи стоит имя заметки без ".md".

#ifndef ZAMETTI_NOTE_TREE_H
#define ZAMETTI_NOTE_TREE_H

#include <QAbstractItemModel>
#include <QModelIndex>
#include <QString>
#include <QSet>
#include <QStyledItemDelegate>
#include <QTreeView>

#include <memory>
#include <vector>

namespace zametti {

class NoteTreeModel : public QAbstractItemModel {
    Q_OBJECT

public:
    // Узел дерева. Деталь реализации, но лежит в public: его строят свободные
    // функции в note_tree.cpp.
    struct Node;

    explicit NoteTreeModel(const QString& root, QObject* parent = nullptr);
    ~NoteTreeModel() override;

    QModelIndex index(int row, int column, const QModelIndex& parent) const override;
    QModelIndex parent(const QModelIndex& child) const override;
    int rowCount(const QModelIndex& parent) const override;
    int columnCount(const QModelIndex& parent) const override;
    QVariant data(const QModelIndex& index, int role) const override;

    // Путь к файлу или пустая строка, если это каталог.
    QString filePath(const QModelIndex& index) const;
    // Путь узла, каталога или файла. Нужен, чтобы запоминать раскрытые ветки.
    QString nodePath(const QModelIndex& index) const;
    bool isDirectory(const QModelIndex& index) const;
    // Индекс узла по пути; недействителен, если его нет в дереве.
    QModelIndex indexForPath(const QString& path) const;

    bool isEmpty() const;

    // Раскрытость ветки знает представление, а рисовать значок должна модель —
    // поэтому она сообщает о ней сюда.
    void setExpanded(const QModelIndex& index, bool expanded);

    // Где считать корнем дерева. Порядок: заданное в конфиге; иначе ближайший
    // каталог вверх от заметки, помеченный как хранилище (.obsidian или .git) —
    // так открытая из глубины заметка всё равно показывает всё дерево; иначе
    // просто каталог самой заметки.
    static QString rootFor(const QString& filePath, const QString& configuredRoot);

private:
    std::unique_ptr<Node> root_;
    QSet<QString> expanded_;
};

// Высота строки в дереве. Отдельного способа задать её у QTreeView нет: он
// спрашивает размер у делегата, поэтому множитель применяется здесь.
class NoteTreeDelegate : public QStyledItemDelegate {
    Q_OBJECT

public:
    using QStyledItemDelegate::QStyledItemDelegate;

    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override;
};

// Дерево без треугольников ветвления: раскрытость видна по значку папки, а два
// указателя на одно и то же только шумят. Убрать их иначе нельзя — QTreeView
// рисует их сам, отдельной настройки нет.
class NoteTreeView : public QTreeView {
    Q_OBJECT

public:
    explicit NoteTreeView(QWidget* parent = nullptr);

protected:
    void drawBranches(QPainter* painter, const QRect& rect,
                      const QModelIndex& index) const override;
};

}  // namespace zametti

#endif  // ZAMETTI_NOTE_TREE_H
