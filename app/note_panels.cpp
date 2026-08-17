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

NotePanels::NotePanels(std::shared_ptr<ZStorage> storage, QObject* parent)
    : QObject(parent), model_(std::move(storage)), keyWalk_(std::make_shared<KeyWalk>()) {
    // Левая панель — только папки (этап 4). Заметки живут в средней колонке;
    // из дерева они не пропадают, но наружу не показываются.
    model_.setFoldersOnly(model_.isStore());

    tree_.setModel(&model_);
    tree_.setHeaderHidden(true);
    tree_.setEditTriggers(model_.isStore() ? QAbstractItemView::EditKeyPressed
                                           : QAbstractItemView::NoEditTriggers);
    if (model_.isStore()) {
        // Не InternalMove: заметку тащат из средней колонки, а это другая
        // модель — для дерева такой перенос внешний.
        tree_.setDragDropMode(QAbstractItemView::DragDrop);
        tree_.setDefaultDropAction(Qt::MoveAction);
        tree_.setDropIndicatorShown(true);
        tree_.setAcceptDrops(true);
    }
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
    tree_.setVisible(visible);
    if (model_.isStore()) middle_.setVisible(visible);
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
    if (!file.isEmpty() && file != currentNote_) {
        openedByFolderPick_ = file;
        emit noteChosen(file, !keyWalk_->walking);
    }
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

void NotePanels::showNote(const QString& file, bool force) {
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
    QModelIndex folder = model_.folderIndexForNote(QFileInfo(file).completeBaseName());
    if (!folder.isValid() && force) folder = model_.indexForPath(model_.nodePath(QModelIndex()));
    bool moved = false;
    if (folder.isValid()) {
        // КУРСОР НЕ ОТБИРАЕМ У ТОГО, КТО ЕГО ТОЛЬКО ЧТО ПОСТАВИЛ. Человек
        // щёлкнул по папке — курсор встал на неё, средняя колонка показала её
        // заметки и открыла первую. Заметка эта лежит, случается, в подпапке, и
        // переставлять курсор туда значит увести его из папки, в которую он
        // только что ткнул. Поэтому курсор двигается только если он СНАРУЖИ
        // этой ветки — заметку открыли из поиска, из середины, — и показать,
        // где она лежит, надо. Само правило — shouldMoveTreeCursor (note_tree.h):
        // там оно названо, объяснено и проверено набором.
        const bool byFolderPick = !openedByFolderPick_.isEmpty() && file == openedByFolderPick_;
        openedByFolderPick_.clear();
        bool insideCurrent = false;
        for (QModelIndex up = folder; up.isValid() && !insideCurrent; up = up.parent())
            insideCurrent = up == tree_.currentIndex();
        moved = force || shouldMoveTreeCursor(byFolderPick, tree_.hasFocus(), insideCurrent);

        // Не QSignalBlocker: замерено пробником, что с заглушенными сигналами
        // курсор дерева не переставляется вовсе; поэтому сигнал идёт как
        // обычно, а его обработчик на время выключен флагом.
        revealing_ = true;
        expandAncestors(tree_, folder);
        if (moved) {
            if (force) tree_.expand(folder);   // человек идёт смотреть, что внутри
            tree_.setCurrentIndex(folder);
            tree_.scrollTo(folder);
        }
        revealing_ = false;
    }

    // ПОРЯДОК ВЫБРАННОЙ ПАПКИ — и когда курсор поставили мы. Обработчик выбора
    // на это время выключен, значит и порядок с кнопками надо догнать здесь;
    // иначе метка папки на первом экране не действовала бы, и владелец видел
    // бы «метка работает через раз».
    if (moved) syncSort();

    // Заметки может не быть в списке вовсе — так бывает, когда из поиска
    // открыли заметку из другой папки. Тогда список пересобирается по той
    // папке, где она лежит: пустая средняя колонка рядом с открытым текстом
    // читалась бы как потеря места.
    QModelIndex row = list_.indexForPath(file);
    if (!row.isValid() && folder.isValid()) {
        list_.setRows(model_.notesInSubtree(folder));
        row = list_.indexForPath(file);
    }
    if (!row.isValid()) return;
    revealing_ = true;
    listView_.setCurrentIndex(row);
    listView_.scrollTo(row, force ? QAbstractItemView::PositionAtCenter
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
    applyPalette(tree_);
    applyPalette(listView_);
    // Делегаты читают настройки прямо при отрисовке — им довольно перерисовки,
    // но размеры строк они считают там же, и без сброса подсказок список
    // остался бы с прежними высотами.
    tree_.doItemsLayout();
    listView_.doItemsLayout();
}

}  // namespace zametti
