#include "note_panels.h"

#include "note_view.h"

#include <QAbstractItemView>
#include <QEvent>
#include <QFileInfo>
#include <QItemSelectionModel>
#include <QKeyEvent>
#include <QShortcut>
#include <QSignalBlocker>
#include <QVBoxLayout>

namespace zametti {

class NotePanels::KeyWalk : public QObject {
public:
    bool walking = false;
    bool eventFilter(QObject*, QEvent* event) override {
        if (event->type() == QEvent::KeyPress) {
            const int key = static_cast<QKeyEvent*>(event)->key();
            walking = key == Qt::Key_Up || key == Qt::Key_Down || key == Qt::Key_PageUp ||
                      key == Qt::Key_PageDown || key == Qt::Key_Home || key == Qt::Key_End;
        } else if (event->type() == QEvent::MouseButtonPress) {
            walking = false;
        }
        return false;
    }
};

// ЧЕМ ПАНЕЛИ ОТЛИЧАЮТСЯ ПРИ ХРАНИЛИЩЕ И БЕЗ НЕГО — В ОДНОМ МЕСТЕ. Раньше эти
// четыре решения принимались в конструкторе и больше никогда: пока хранилище
// открывалось ровно один раз за запуск, этого хватало. Теперь его переключают
// на ходу, и решения обязаны приниматься заново — той же функцией, а не
// повторённые второй раз рядом.
void NotePanels::applyStoreMode() {
    const bool store = model_.isStore();
    // Левая панель — только папки (этап 4). Заметки живут в средней колонке;
    // из дерева они не пропадают, но наружу не показываются.
    model_.setFoldersOnly(store);
    tree_.setEditTriggers(store ? QAbstractItemView::EditKeyPressed
                                : QAbstractItemView::NoEditTriggers);
    // Не InternalMove: заметку тащат из средней колонки, а это другая модель —
    // для дерева такой перенос внешний. Без хранилища переносить нечего и
    // некуда, и приём перетаскивания снимается ЯВНО: оставшись включённым от
    // прошлого хранилища, он принял бы бросок в пустое дерево.
    tree_.setDragDropMode(store ? QAbstractItemView::DragDrop
                                : QAbstractItemView::NoDragDrop);
    tree_.setDefaultDropAction(store ? Qt::MoveAction : Qt::IgnoreAction);
    tree_.setDropIndicatorShown(store);
    tree_.setAcceptDrops(store);
}

void NotePanels::setStorage(std::shared_ptr<ZStorage> storage) {
    // Порядок важен: сперва модель переезжает (её сброс опустошит вид), потом
    // повторяются решения «хранилище или нет», потом забывается открытая
    // заметка — она была из прежнего хранилища.
    model_.setStorage(std::move(storage));
    applyStoreMode();
    currentNote_.clear();
    list_.setRows({});
    // Средняя колонка есть только у хранилища. За «спрятаны ли панели вообще»
    // отвечает кнопка тулбара, и её решение видно по дереву — на него и
    // равняемся, как это делает setVisible ниже.
    middle_.setVisible(model_.isStore() && wanted_);
}

NotePanels::NotePanels(std::shared_ptr<ZStorage> storage, QObject* parent)
    : QObject(parent), model_(std::move(storage)), keyWalk_(std::make_shared<KeyWalk>()) {
    tree_.setModel(&model_);
    tree_.setHeaderHidden(true);
    applyStoreMode();
    tree_.setUniformRowHeights(true);
    tree_.setItemDelegate(&treeDelegate_);
    tree_.setContextMenuPolicy(Qt::CustomContextMenu);
    connect(&tree_, &QTreeView::expanded, this,
            [this](const QModelIndex& i) { model_.setExpanded(i, true); });
    connect(&tree_, &QTreeView::collapsed, this,
            [this](const QModelIndex& i) { model_.setExpanded(i, false); });

    // Средняя колонка: плоский список заметок выбранной папки.
    {
        auto* layout = new QVBoxLayout(&middle_);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(0);
        listView_.setModel(&list_);
        listView_.setItemDelegate(&listDelegate_);
        listView_.setUniformItemSizes(false);   // высота строки зависит от сниппета
        // Горизонтальной прокрутки в списке быть не должно: строка и так
        // укорачивается по ширине, а полоса отъедала правый край — даты
        // обрезались (замерено на снимке).
        listView_.setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        listView_.setSelectionMode(QAbstractItemView::SingleSelection);
        listView_.setDragEnabled(true);   // перетащить заметку на папку слева
        listView_.setDragDropMode(QAbstractItemView::DragOnly);
        listView_.setContextMenuPolicy(Qt::CustomContextMenu);
        layout->addWidget(&listView_, 1);
    }

    tree_.installEventFilter(keyWalk_.get());
    listView_.installEventFilter(keyWalk_.get());

    // Выбор папки слева: сперва её порядок, потом её список. Обратный порядок
    // дал бы список, отсортированный по прежней папке. Дерево при этом не
    // трогается вовсе — порядок в левой панели принадлежит папкам, а не
    // выбранной строке.
    connect(tree_.selectionModel(), &QItemSelectionModel::currentChanged, this,
            [this](const QModelIndex& index, const QModelIndex&) {
                if (revealing_) return;
                if (model_.isStore()) {
                    folderPicked(index);
                    return;
                }
                // Вне хранилища панель одна: заметки живут в дереве.
                openFromPanel(model_.filePath(index));
            });
    // Щелчок по УЖЕ выбранной папке. Курсор мог встать на неё сам — так
    // работает подсветка открытой заметки, — и тогда currentChanged больше не
    // сработает, а сузить список надо: человек ткнул в папку явно и ждёт
    // увидеть только её заметки. Программная перестановка курсора сюда не
    // попадает: clicked приходит только от настоящего щелчка.
    connect(&tree_, &QAbstractItemView::clicked, this, [this](const QModelIndex& index) {
        if (model_.isStore()) folderPicked(index);
    });
    // Выбор строки списка открывает заметку. Фокус переезжает в текст — кроме
    // ходьбы стрелками (правило средней колонки).
    connect(listView_.selectionModel(), &QItemSelectionModel::currentChanged, this,
            [this](const QModelIndex& index, const QModelIndex&) {
                if (revealing_) return;
                openFromPanel(list_.pathAt(index));
            });
    // Enter в списке — перейти к правке.
    connect(&listView_, &QAbstractItemView::activated, this,
            [this](const QModelIndex&) { emit editRequested(); });
    // Строка изменилась на месте (сохранение, живой заголовок) — список вслед.
    connect(&model_, &NoteTreeModel::noteRowChanged, this,
            [this](const QString& id) { list_.updateRow(model_.rowOf(id)); });
    // Дерево перестроено (каталог изменился, порядок корня сменился) — список
    // по текущей папке заново, открытая заметка в нём остаётся выделенной.
    connect(&tree_, &NoteTreeView::rebuilt, this, &NotePanels::refillAfterRebuild);

    // Del в дереве бьёт по папке, Del в списке — по заметке. Каждый ярлык висит
    // на своём виджете: иначе Del из дерева удалял буквы заголовка в редакторе.
    {
        auto* del = new QShortcut(QKeySequence::Delete, &tree_);
        del->setContext(Qt::WidgetWithChildrenShortcut);
        connect(del, &QShortcut::activated, this,
                [this] { emit deleteRequested(model_.idOf(tree_.currentIndex())); });
    }
    {
        auto* del = new QShortcut(QKeySequence::Delete, &listView_);
        del->setContext(Qt::WidgetWithChildrenShortcut);
        connect(del, &QShortcut::activated, this,
                [this] { emit deleteRequested(list_.idAt(listView_.currentIndex())); });
    }
}

NotePanels::~NotePanels() {
    // Виджеты не дети друг друга и не дети окна: снимаем модель у дерева до
    // того, как она умрёт (порядок полей — обратный объявлению).
    tree_.setModel(nullptr);
    listView_.setModel(nullptr);
}

void NotePanels::setVisible(bool visible) {
    wanted_ = visible;
    tree_.setVisible(visible);
    // Средняя колонка есть только у хранилища — и прятать её надо ЯВНО, а не
    // «не показывать»: без хранилища она обязана исчезнуть, даже если панели
    // включены. Раньше её просто не добавляли в сплиттер, и вопрос не стоял.
    middle_.setVisible(visible && model_.isStore());
}

void NotePanels::openFromPanel(const QString& file) {
    if (file.isEmpty() || file == currentNote_) return;
    emit noteChosen(file, !keyWalk_->walking);
}

void NotePanels::setCurrentNote(const QString& file) { currentNote_ = file; }

// Средняя колонка наполняется по выбранной слева папке. Открытая заметка, если
// она в этом поддереве, остаётся выбранной — переключение папки не должно
// уводить человека с того, что он читает; иначе открывается первая заметка
// списка (так ведёт себя Apple Notes).
void NotePanels::fillList(const QModelIndex& folder, bool openFirst) {
    if (!model_.isStore()) return;
    // У виртуальной папки порядок свой — тот, в котором её завели, — и
    // переключателю сортировки он не подчиняется: дат у документации нет.
    // Ставится ДО setRows: сортировка случается внутри него.
    list_.setFixedOrder(model_.isVirtualFolder(folder));
    list_.setRows(model_.notesInSubtree(folder));
    const QModelIndex keep = list_.indexForPath(currentNote_);
    if (keep.isValid()) {
        const QSignalBlocker blocked(listView_.selectionModel());
        listView_.setCurrentIndex(keep);
        listView_.scrollTo(keep);
        return;
    }
    if (!openFirst || list_.rowCount() == 0) return;
    const QModelIndex first = list_.index(0, 0);
    {
        const QSignalBlocker blocked(listView_.selectionModel());
        listView_.setCurrentIndex(first);
    }
    const QString file = list_.pathAt(first);
    if (!file.isEmpty() && file != currentNote_) emit noteChosen(file, !keyWalk_->walking);
}

void NotePanels::folderPicked(const QModelIndex& index) {
    syncSort();
    fillList(index, true);
}

void NotePanels::refillAfterRebuild() {
    if (!model_.isStore()) return;
    // Порядок мог смениться вместе с деревом (метка папки), список — тоже.
    syncSort();
    fillList(tree_.currentIndex(), false);
}

void NotePanels::selectNote(const QString& file) {
    const QModelIndex row = list_.indexForPath(file);
    if (!row.isValid()) return;
    const QSignalBlocker blocked(listView_.selectionModel());
    listView_.setCurrentIndex(row);
    listView_.scrollTo(row);
}

void NotePanels::showNote(const QString& file, bool primary) {
    if (file.isEmpty()) return;
    currentNote_ = file;
    if (!model_.isStore()) {
        // Вне хранилища заметки живут в дереве: показать — значит встать на неё.
        revealing_ = true;
        if (tree_.setCurrentPath(file, /*quiet=*/true))
            tree_.scrollTo(tree_.currentIndex(), QAbstractItemView::PositionAtCenter);
        revealing_ = false;
        return;
    }
    // ДВА ВЫДЕЛЕНИЯ (решение владельца). Первичное — курсор дерева — папка, по
    // которой человек ткнул ЯВНО; она задаёт состав и порядок средней колонки, и
    // показ открытой заметки её НЕ ТРОГАЕТ: ни курсор, ни список, ни порядок.
    // Порядок папке критически важен (дневник — по дате создания, «все заметки»
    // — по правке), и заметка, открытая из поиска или истории, не вправе его
    // подменить. Папка открытой заметки — вторичное выделение: пунктирная
    // рамка, предки раскрыты — «лежит здесь; хочешь перейти — ткни».
    //
    // primary — только когда первичного выделения ещё нет ни у кого (старт):
    // им становится папка открытой заметки, и список наполняется по ней.
    QModelIndex folder = model_.folderIndexForNote(QFileInfo(file).completeBaseName());
    if (!folder.isValid() && primary) folder = model_.indexForPath(model_.nodePath(QModelIndex()));
    revealing_ = true;
    if (folder.isValid()) {
        expandAncestors(tree_, folder);
        model_.setSecondaryPath(model_.nodePath(folder));
        if (primary) {
            tree_.setCurrentIndex(folder);
            tree_.scrollTo(folder);
        }
    }
    revealing_ = false;
    if (primary) {
        syncSort();
        fillList(folder, false);
    }

    // В списке — выделить строку, если заметка в нём есть. Нет — значит, человек
    // смотрит другую папку; список его, и он остаётся.
    const QModelIndex row = list_.indexForPath(file);
    if (!row.isValid()) return;
    revealing_ = true;
    listView_.setCurrentIndex(row);
    listView_.scrollTo(row, primary ? QAbstractItemView::PositionAtCenter
                                    : QAbstractItemView::EnsureVisible);
    revealing_ = false;
}

QString NotePanels::currentFolderId() const {
    return model_.folderIdFor(tree_.currentIndex());
}

// --- порядок сортировки --------------------------------------------------------
//
// Дерево сортируется по папкам: у каждой свой порядок (своя метка → родительская
// → переключатель корня), и выбор строки его не меняет. Смена ВЫБОРА не трогает
// дерево вовсе: меняются только средняя колонка — она показывает мир выбранной
// папки и потому идёт её порядком — и кнопки тулбара, которые этот порядок
// называют.

void NotePanels::setRootSort(SortOrder order) {
    rootSort_ = order;
    // Пересортировка модели — сброс; дерево вернёт раскрытость и курсор само,
    // список наполнится по rebuilt. Порядок тот же — модель ничего не делает,
    // а список и кнопки догоняем явно.
    model_.setRootSort(order);
    syncSort();
}

SortOrder NotePanels::currentSort(bool* fromMark) const {
    return model_.effectiveSortFor(currentFolderId(), rootSort_, fromMark);
}

void NotePanels::syncSort() {
    bool fromMark = false;
    const SortOrder order = currentSort(&fromMark);
    list_.setSortOrder(order);
    emit sortShown(order, fromMark);
}

// --- облик ---------------------------------------------------------------------

void NotePanels::setSidebarFont(const QFont& font) {
    tree_.setFont(font);
    listView_.setFont(font);
}

void NotePanels::refreshAppearance() {
    // Фон боковых панелей — свой: у тёмных тем колонка обычно темнее листа.
    applyPalette(tree_, settings().ui().sidebarBackground());
    applyPalette(listView_, settings().ui().sidebarBackground());
    // Делегаты читают настройки прямо при отрисовке — им довольно перерисовки,
    // но размеры строк они считают там же, и без сброса подсказок список
    // остался бы с прежними высотами.
    tree_.doItemsLayout();
    listView_.doItemsLayout();
}

}  // namespace zametti
