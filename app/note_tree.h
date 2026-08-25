// Дерево заметок для боковой панели.
//
// Модель своя, а не QFileSystemModel. У того две беды для нашего случая: он
// асинхронный (каталог догружается уже после того, как мы попросили выделить
// текущую заметку) и показывает каталоги, в которых заметок нет вовсе. Здесь
// дерево строится один раз обходом каталога — для сотен файлов это мгновенно, —
// пустые ветки отбрасываются, а в подписи стоит имя заметки без ".md".

#ifndef ZAMETTI_NOTE_TREE_H
#define ZAMETTI_NOTE_TREE_H

#include "sort_order.h"
#include "zstorage.h"

#include <QAbstractItemModel>
#include <QMimeData>
#include <QModelIndex>
#include <QString>
#include <QSet>
#include <QStyledItemDelegate>
#include <QTreeView>

#include <memory>
#include <optional>
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
    // Дата создания — единственная неподвижная хронология заметки: правка
    // старой записи её не двигает. Ради неё и затевался этап 13 (дневник).
    QString created;
};

// Показать строку в дереве, не трогая её собственную раскрытость: раскрываются
// только ПРЕДКИ.
//
// Отдельной функцией с именем, а не тремя строчками на месте, потому что
// разница между «раскрыть предков» и «раскрыть заодно и саму строку» стоила
// владельцу отдельной беды, и на глаз она не видна.
//
// Щелчок по строке папки Qt сама переключает её раскрытость — на ОТПУСКАНИИ
// мыши, сигналом clicked. Окно же успевает вмешаться раньше, между нажатием и
// отпусканием: нажатие переставило курсор, средняя колонка открыла первую
// заметку папки, и отложенный fileChanged добрался до показа строки. Раскрой
// тут саму папку — и на один щелчок придётся два переключения: мы раскрыли,
// Qt тут же закрыла. Выглядит это как «папка открывается и мгновенно
// захлопывается, а со второго раза работает»: со второго раза курсор уже стоит
// на ней, заметка не переоткрывается, и переключение остаётся одно.
void expandAncestors(QTreeView& tree, const QModelIndex& row);

class NoteTreeModel : public QAbstractItemModel {
    Q_OBJECT

public:
    // Узел дерева. Деталь реализации, но лежит в public: его строят свободные
    // функции в note_tree.cpp.
    struct Node;

    // Дерево — ПРОЕКЦИЯ каталога хранилища (ZStorage): диск оно не читает.
    explicit NoteTreeModel(std::shared_ptr<ZStorage> storage, QObject* parent = nullptr);
    // Удобство наборов и утилит: заводит хранилище само.
    explicit NoteTreeModel(const QString& root, QObject* parent = nullptr);
    std::shared_ptr<ZStorage> storage() const { return storage_; }
    ~NoteTreeModel() override;

    // Плоское ли это хранилище (метка — каталог .zametti). В нём дерево
    // строится не по файловой системе, а по метаданным: каталог — это обычная
    // заметка, у которой есть дети (parent в мете ребёнка).
    static bool isStoreRoot(const QString& dir) { return ZStorage::isStoreRoot(dir); }
    bool isStore() const { return store_; }

    // Id заметки узла (пусто вне хранилища). Корневой индекс — пустой id.
    QString idOf(const QModelIndex& index) const;

    // Чистый заголовок узла — без пометок починки вроде «[сирота]».
    QString titleOf(const QModelIndex& index) const;

    // Перечитать хранилище; дерево перестроится по его сигналу. Раскрытость и
    // выбор бережёт представление (NoteTreeView через сброс модели).
    void refresh();

    // Перечитать одну заметку: заголовок, сниппет и дату. Нужен после
    // сохранения и после внешней правки — пересканировать всё хранилище ради
    // одной изменившейся заметки незачем.
    void refreshNote(const QString& path);
    // Перестроить по каталогу, какой он сейчас, — приходит из ZStorage::catalogChanged.
    void rebuild();
    // Обновить одну строку по каталогу — из ZStorage::noteChanged.
    void refreshRow(const QString& id);

    // Левая панель показывает только папки (этап 4). Заметки из дерева при
    // этом не исчезают — они нужны средней колонке, — но наружу, через
    // интерфейс модели, не видны.
    void setFoldersOnly(bool on);

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

    // Порядок КОРНЯ — он же запасной для всех, у кого нет ни своей метки, ни
    // помеченного предка. Это переключатель тулбара, и только он.
    //
    // ПОРЯДОК ПРИНАДЛЕЖИТ ПАПКЕ, а не выбранной строке: каждая папка
    // упорядочивает своих детей по своей метке, у кого метки нет — по
    // родительской. Выбор папки дерево не трогает вовсе; левая панель
    // перекладывается ровно двумя способами — сменой этого переключателя и
    // правкой метки (после которой хранилище перечитывается).
    //
    // Прежде здесь стоял «действующий порядок точки обзора», один на всё
    // дерево. Владелец увидел, во что это выливается: щелчок по папке,
    // помеченной другим порядком, перекладывал ВСЮ левую панель, и строка под
    // курсором оказывалась чужой.
    void setRootSort(SortOrder order);

    // Метка САМОЙ этой папки, если она есть: ключ `sort` из её шапки. Пусто —
    // метки нет или значение чужое (тогда о нём уже пожаловались в stderr).
    std::optional<SortOrder> explicitSortOf(const QString& id) const;
    // Действующий порядок для папки: своя метка → ближайший помеченный предок →
    // fallback (переключатель корня). Второе значение — откуда взяли: true,
    // если порядок задан меткой, false — если это переключатель.
    SortOrder effectiveSortFor(const QString& id, SortOrder fallback,
                               bool* fromMark = nullptr) const;

    // Ближайшая папка вверх от узла: сам узел, если он папка, иначе его
    // родитель-папка; корень — пустой id. Создание всегда целится сюда:
    // заметка никогда не становится папкой, как и наоборот (правило
    // владельца), поэтому «в текущей папке» — единственное место.
    QString folderIdFor(const QModelIndex& index) const;

    // Всё, что лежит в Архиве, — id заметок и папок, сверху вниз. Архив —
    // ВИРТУАЛЬНАЯ папка: файла за ней нет, она собирается из помеченных
    // (`archived: yes` в шапке), а `parent` у заметки остаётся прежним.
    QStringList archivedIds() const;
    // Это сама строка «Архив»?
    bool isArchiveBox(const QModelIndex& index) const;
    // Лежит ли узел в Архиве (сам помечен или помечен кто-то выше).
    bool inArchive(const QModelIndex& index) const;
    bool inArchiveId(const QString& id) const;
    // Помечена ли архивной именно эта заметка (а не её предок).
    bool isArchivedId(const QString& id) const;
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

    // ВТОРИЧНОЕ ВЫДЕЛЕНИЕ — папка открытой заметки (решение владельца). Первичное
    // выделение (курсор дерева) — папка, по которой человек ткнул ЯВНО; она и
    // только она задаёт состав и порядок средней колонки. Открытая заметка может
    // лежать где угодно — из поиска, из истории, из «всех заметок», — и её папка
    // показывается пунктирной незакрашенной рамкой: «заметка, которую ты открыл,
    // лежит здесь; хочешь перейти — ткни». Два выделения различимы глазом и
    // могут совпадать. Роль читает делегат (NoteTreeDelegate); хранится путём,
    // чтобы пережить перестройку.
    enum Roles { SecondaryRole = Qt::UserRole + 1 };
    void setSecondaryPath(const QString& path);
    QString secondaryPath() const { return secondaryPath_; }

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
    SortOrder rootSort_ = defaultOrder(SortKey::Modified);
    std::shared_ptr<ZStorage> storage_;
    std::shared_ptr<Node> root_;
    QSet<QString> expanded_;
    QString secondaryPath_;
};

// Высота строки в дереве. Отдельного способа задать её у QTreeView нет: он
// спрашивает размер у делегата, поэтому множитель применяется здесь.
class NoteTreeDelegate : public QStyledItemDelegate {
    Q_OBJECT

public:
    using QStyledItemDelegate::QStyledItemDelegate;

    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override;
    // Пунктирная рамка вторичного выделения (NoteTreeModel::SecondaryRole).
    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override;
};

// КУРСОР ДЕРЕВА ПОКАЗ ОТКРЫТОЙ ЗАМЕТКИ НЕ ДВИГАЕТ НИКОГДА (решение владельца,
// сессия 4 refactor2; прежнее правило shouldMoveTreeCursor снято). Курсор —
// первичное выделение, оно принадлежит человеку и задаёт среднюю колонку; папка
// открытой заметки — вторичное (NoteTreeModel::setSecondaryPath).

// Дерево без треугольников ветвления: раскрытость видна по значку папки, а два
// указателя на одно и то же только шумят. Убрать их иначе нельзя — QTreeView
// рисует их сам, отдельной настройки нет.
class NoteTreeView : public QTreeView {
    Q_OBJECT

public:
    explicit NoteTreeView(QWidget* parent = nullptr);

    // РАСКРЫТЫЕ ВЕТКИ И ТЕКУЩАЯ СТРОКА ПЕРЕЖИВАЮТ СБРОС МОДЕЛИ. Дерево строится
    // заново на каждую структурную новость каталога (модель сбрасывается
    // целиком), а человек не должен видеть, как панель складывается и теряет
    // курсор. Состояние берётся ПУТЯМИ (nodePath) — индексы после сброса
    // недействительны — и возвращается с заглушенными сигналами выбора: это
    // не выбор человека, и заметку переоткрывать не надо. Прежде это делало
    // окно после каждой операции (refreshTree в main.cpp), и забыть было легко.
    void setModel(QAbstractItemModel* model) override;
    // Раскрытые папки путями (для state.json) и обратно.
    QStringList expandedDirs() const;
    void restoreExpanded(const QStringList& dirs);
    // Путь текущей строки (пусто — ничего не выбрано).
    QString currentPath() const;
    // Поставить курсор на строку по пути, раскрыв предков; тихо — без сигналов
    // выбора. Ложь — такой строки нет.
    bool setCurrentPath(const QString& path, bool quiet);

signals:
    // Модель перестроена, раскрытость и курсор возвращены: кто наполняет
    // список по текущей папке — теперь ему пора.
    void rebuilt();

protected:
    void drawBranches(QPainter* painter, const QRect& rect,
                      const QModelIndex& index) const override;
    // Состояние строки запоминается на НАЖАТИИ — переключаем от него.
    void mousePressEvent(QMouseEvent* event) override;

private:
    // Какую строку нажали и была ли она раскрыта В ТОТ МОМЕНТ.
    //
    // Раз треугольников нет, папку раскрывает обычный щелчок — а щелчок это
    // сигнал clicked, и приходит он на ОТПУСКАНИИ. Между нажатием и
    // отпусканием успевает пройти целый круг событий: нажатие переставило
    // курсор, средняя колонка открыла первую заметку папки, окно показало её в
    // дереве и по дороге раскрыло папку. Спроси мы раскрытость в этот момент —
    // увидим «уже открыта» и закроем. Владелец видел это как «папка
    // открывается и мгновенно захлопывается, а со второго раза работает»: со
    // второго раза курсор уже стоял на ней, заметка не переоткрывалась, и
    // раскрывать её было некому.
    //
    // Поэтому переключаем от того, ЧТО ЧЕЛОВЕК ВИДЕЛ, когда нажимал. Заодно
    // это верно и для любой другой правки дерева, случившейся между нажатием и
    // отпусканием: щелчок значит ровно то, что человеку было видно.
    QPersistentModelIndex pressedRow_;
    bool pressedExpanded_ = false;
    // И БЫЛА ЛИ СТРОКА УЖЕ ВЫБРАНА. Закрывать папку почти никогда не нужно, а
    // переходить на неё — нужно постоянно, и одним щелчком делать оба дела
    // нельзя: человек шёл посмотреть, что внутри, а папка захлопывалась.
    //
    // Отсюда ритуал (решение владельца): первый щелчок ВЫБИРАЕТ папку и
    // раскрывает её, если она была закрыта; открытую он не трогает. И только
    // щелчок по УЖЕ ВЫБРАННОЙ папке переключает раскрытость — то есть закрыть
    // её можно, но для этого надо ткнуть дважды, осознанно.
    bool pressedWasCurrent_ = false;

    // Что бережём через сброс модели.
    QStringList keptExpanded_;
    QString keptCurrent_;
    void collectExpanded(const QModelIndex& parent, QStringList& out) const;
};

}  // namespace zametti

#endif  // ZAMETTI_NOTE_TREE_H
