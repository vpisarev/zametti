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
#include <QMimeData>
#include <QModelIndex>
#include <QString>
#include <QSet>
#include <QStyledItemDelegate>
#include <QTreeView>

#include <memory>
#include <vector>

namespace zametti {

// Строка средней колонки: заметка так, как её видит человек — заголовок,
// начало текста и дата правки. Собирается при скане хранилища: перечитывать
// файлы на каждую перерисовку списка незачем (замер этапа 4: полный проход с
// разбором — 7 мс на 271 заметке, 80 мс на 2710).
struct NoteRow {
    QString id;
    QString path;
    QString title;
    QString snippet;
    QString modified;   // ISO из меты; пусто — не знаем
};

class NoteTreeModel : public QAbstractItemModel {
    Q_OBJECT

public:
    // Узел дерева. Деталь реализации, но лежит в public: его строят свободные
    // функции в note_tree.cpp.
    struct Node;

    explicit NoteTreeModel(const QString& root, QObject* parent = nullptr);
    ~NoteTreeModel() override;

    // Плоское ли это хранилище (метка — каталог .zametti). В нём дерево
    // строится не по файловой системе, а по метаданным: каталог — это обычная
    // заметка, у которой есть дети (parent в мете ребёнка).
    static bool isStoreRoot(const QString& dir);
    bool isStore() const { return store_; }

    // Id заметки узла (пусто вне хранилища). Корневой индекс — пустой id.
    QString idOf(const QModelIndex& index) const;

    // Чистый заголовок узла — без пометок починки вроде «[сирота]».
    QString titleOf(const QModelIndex& index) const;

    // Перестроить дерево по текущему содержимому. Выбор и раскрытость чинит
    // вызывающий: у модели нет доступа к представлению.
    void refresh();

    // Перечитать одну заметку: заголовок, сниппет и дату. Нужен после
    // сохранения и после внешней правки — пересканировать всё хранилище ради
    // одной изменившейся заметки незачем.
    void refreshNote(const QString& path);

    // Левая панель показывает только папки (этап 4). Заметки из дерева при
    // этом не исчезают — они нужны средней колонке, — но наружу, через
    // интерфейс модели, не видны.
    void setFoldersOnly(bool on);
    bool foldersOnly() const { return foldersOnly_; }

    // Все заметки поддерева, плоско и в порядке текущей сортировки: средняя
    // колонка не группирует по подпапкам. Недействительный индекс — всё
    // хранилище. Корзина в общий список не попадает (как в Apple Notes);
    // изнутри самой корзины показывается её содержимое.
    std::vector<NoteRow> notesInSubtree(const QModelIndex& index) const;

    // Строка одной заметки — средней колонке после сохранения.
    NoteRow rowOf(const QString& id) const;

    // Индекс папки, в которой лежит заметка (для подсветки в левой панели).
    QModelIndex folderIndexForNote(const QString& noteId) const;

    // Запросы по id. Заметки в левой панели не показываются, а операции над
    // ними (корзина, восстановление, перенос) приходят из средней колонки —
    // там индексы совсем другой модели. Поэтому спрашивают по id, а не по
    // QModelIndex: индекс скрытого узла был бы враньём для представления.
    bool hasNote(const QString& id) const;
    bool isFolderId(const QString& id) const;
    bool inTrashId(const QString& id) const;
    QString parentIdOf(const QString& id) const;
    QString titleOfId(const QString& id) const;
    // Заголовки папок от корня до родителя заметки: по ним «Восстановить»
    // пересоздаёт цепочку, если прежних папок уже нет.
    QStringList ancestorTitles(const QString& id) const;
    int childCountOf(const QString& id) const;
    // Всё поддерево этого узла, сам узел не входит. Порядок — от самых
    // глубоких к верхним: так их можно удалять подряд, не оставляя папку с
    // детьми. Нужен «очистить корзину»: там надо и перечислить, и удалить.
    QStringList descendantIdsOf(const QString& id) const;
    QString pathOfId(const QString& id) const;
    // Папка с таким заголовком внутри папки parentId (пусто — корень); пусто,
    // если её нет. Нужна восстановлению из корзины: путь запомнен именами.
    QString childFolderByTitle(const QString& parentId, const QString& title) const;
    // Первая открываемая заметка обходом в глубину; пусто — в хранилище нет
    // ни одной. Папки не в счёт: они не открываются.
    QString firstNoteId() const;
    // Сосед по списку, на который уйдёт выделение после удаления: сначала
    // вниз, потом вверх, в порядке текущей сортировки.
    QString neighbourOf(const QString& id) const;

    // Живая подпись: заголовок правится в редакторе — дерево обновляется, не
    // дожидаясь ни сохранения, ни пересборки.
    void updateTitle(const QString& filePath, const QString& title);

    // Сортировка братьев: по последней правке (свежие сверху; у каталога —
    // самая свежая правка в поддереве) или по имени (каталоги первыми).
    // Корзина всегда в самом низу корня.
    enum class SortMode { ByModified, ByName };
    void setSortMode(SortMode mode);
    SortMode sortMode() const { return sortMode_; }

    // Ближайшая папка вверх от узла: сам узел, если он папка, иначе его
    // родитель-папка; корень — пустой id. Создание всегда целится сюда:
    // заметка никогда не становится папкой, как и наоборот (правило
    // владельца), поэтому «в текущей папке» — единственное место.
    QString folderIdFor(const QModelIndex& index) const;

    // Id заметки-корзины (мета-ключ role: trash); пусто, если её ещё нет.
    QString trashId() const;
    // Лежит ли узел в поддереве корзины.
    bool inTrash(const QModelIndex& index) const;
    // Является ли candidate самим узлом id или его потомком: перенос заметки
    // в собственное поддерево запрещён.
    bool isDescendantOf(const QString& candidateId, const QString& id) const;

    // Правки самих файлов модель не делает — только просит: у неё нет ни
    // редактора (открытая заметка правится через него), ни права молча писать.
    Qt::ItemFlags flags(const QModelIndex& index) const override;
    bool setData(const QModelIndex& index, const QVariant& value, int role) override;
    QStringList mimeTypes() const override;
    QMimeData* mimeData(const QModelIndexList& indexes) const override;
    bool canDropMimeData(const QMimeData* data, Qt::DropAction action, int row, int column,
                         const QModelIndex& parent) const override;
    bool dropMimeData(const QMimeData* data, Qt::DropAction action, int row, int column,
                      const QModelIndex& parent) override;
    Qt::DropActions supportedDropActions() const override;



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

signals:
    // F2: человек ввёл новый заголовок. Меняется первый заголовок заметки —
    // выполняет главное окно (через редактор, если заметка открыта).
    void renameRequested(const QString& filePath, const QString& title);
    // Перенос: parentId пуст — в корень.
    void moveRequested(const QString& noteId, const QString& parentId);
    // Заголовок, сниппет или дата одной заметки изменились: средней колонке
    // пора обновить строку, не перестраивая список целиком.
    void noteRowChanged(const QString& noteId);

private:
    void build();
    // Узел, под которым лежит содержимое: в хранилище это видимая строка
    // «All notes», вне хранилища — сам корень.
    const Node* topNode() const;
    QModelIndex indexForNode(const Node* node) const;
    const Node* nodeById(const QString& id) const;
    // Узел заметки по файлу, которым её зовёт редактор. В хранилище ищет по id
    // из имени файла, а не по тексту пути: пути с двух сторон приходят разными
    // дорогами и совпадают не всегда — см. пояснение в note_tree.cpp.
    Node* findByFile(const QString& filePath);

    QString rootPath_;
    bool store_ = false;
    bool foldersOnly_ = false;
    SortMode sortMode_ = SortMode::ByModified;
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
