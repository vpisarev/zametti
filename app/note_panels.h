// NotePanels — левая и средняя колонки окна одним объектом: дерево папок,
// список заметок выбранной папки, ходьба стрелками, показ открытой заметки,
// порядок сортировки выбранной папки.
//
// Зачем объект. Пока это были два виджета и полтора десятка лямбд в main.cpp,
// каждая операция хранилища заканчивалась ритуалом «обновить дерево, вернуть
// раскрытость, вернуть курсор, перезаполнить список, выделить строку» — и
// забыть шаг было делом времени. Теперь:
//
//   * о переменах каталога говорит само хранилище (ZStorage::catalogChanged),
//     дерево перестраивается по его сигналу (NoteTreeModel), а раскрытость и
//     курсор бережёт само дерево (NoteTreeView через сброс модели);
//   * список наполняется по текущей папке дерева, когда дерево перестроено
//     (NoteTreeView::rebuilt) или человек выбрал папку;
//   * панели знают, какая заметка открыта (setCurrentNote), и показывают её
//     по правилу shouldMoveTreeCursor (note_tree.h) — showNote;
//   * наружу уходят намерения человека сигналами: noteChosen — открыть,
//     editRequested — перейти к правке, deleteRequested — убрать, sortShown —
//     что показать на кнопках сортировки.
//
// Панели НЕ ПРАВЯТ ХРАНИЛИЩЕ: создание, перенос, архив, переименование —
// команды окна (пока лямбды main.cpp, дальше MainWindow), панели лишь говорят,
// что человек выбрал. Кроме одного: раскрытость веток модель хранит для
// значков (setExpanded), и это она узнаёт от дерева здесь.
//
// Виджеты дерева и списка отдаются наружу (tree(), listView()): контекстные
// меню и ярлыки окна вешаются на них снаружи — им нужны положение курсора и
// строка под мышью, и тащить их сюда значило бы тащить и все команды окна.

#ifndef ZAMETTI_NOTE_PANELS_H
#define ZAMETTI_NOTE_PANELS_H

#include "note_list.h"
#include "note_tree.h"
#include "sort_order.h"
#include "zstorage.h"

#include <QFont>
#include <QListView>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QWidget>

#include <memory>

namespace zametti {

class NotePanels : public QObject {
    Q_OBJECT

public:
    explicit NotePanels(std::shared_ptr<ZStorage> storage, QObject* parent = nullptr);
    ~NotePanels() override;

    // --- что показывать в сплиттере ------------------------------------------
    NoteTreeView& tree() { return tree_; }
    // Средняя колонка целиком (виджет со списком) — есть только у хранилища.
    QWidget& listPanel() { return middle_; }
    QListView& listView() { return listView_; }
    NoteTreeModel& model() { return model_; }
    NoteListModel& list() { return list_; }
    bool isStore() const { return model_.isStore(); }
    void setVisible(bool visible);

    // --- открытая заметка ---------------------------------------------------
    // Какая заметка открыта в редакторе. Прямой связью с NoteEditor::fileChanged:
    // список сверяется с ней на каждом наполнении.
    void setCurrentNote(const QString& file);
    const QString& currentNote() const { return currentNote_; }
    // ПОКАЗАТЬ ЗАМЕТКУ: раскрыть её папку, поставить курсор дерева по правилу
    // shouldMoveTreeCursor (или без вопросов, если force — возврат из архива,
    // старт), наполнить список её папкой, если заметки в списке нет, и
    // выделить строку. Очередной связью с fileChanged: см. note_tree.h.
    void showNote(const QString& file, bool force = false);
    // Выделить строку списка тихо, если она в нём (соседа после удаления,
    // созданную заметку). Курсор дерева не трогается.
    void selectNote(const QString& file);
    // Папка «текущая» — ближайшая вверх от строки дерева (пусто — корень): для
    // новой заметки, импорта, метки порядка.
    QString currentFolderId() const;

    // --- порядок сортировки -------------------------------------------------
    // Переключатель КОРНЯ (state.json): запасной для всякой папки без метки.
    void setRootSort(SortOrder order);
    SortOrder rootSort() const { return rootSort_; }
    // Порядок, действующий в текущей папке, и от метки ли он.
    SortOrder currentSort(bool* fromMark = nullptr) const;
    // Список — порядком текущей папки; кнопкам — сигнал sortShown.
    void syncSort();

    // --- облик и состояние ---------------------------------------------------
    void setSidebarFont(const QFont& font);
    void refreshAppearance();
    QStringList expandedDirs() const { return tree_.expandedDirs(); }
    void restoreExpanded(const QStringList& dirs) { tree_.restoreExpanded(dirs); }

signals:
    // Человек выбрал заметку: открыть. takeFocus — ложь при ходьбе стрелками
    // (↑/↓ листают панель, а не двигают каретку).
    void noteChosen(const QString& file, bool takeFocus);
    // Enter в списке: выбор сделан, дальше человек хочет печатать.
    void editRequested();
    // Del в дереве (папка) или в списке (заметка).
    void deleteRequested(const QString& id);
    // Что показать на кнопках сортировки.
    void sortShown(SortOrder order, bool fromMark);

protected:
    void fillList(const QModelIndex& folder, bool openFirst);
    void folderPicked(const QModelIndex& index);
    void refillAfterRebuild();
    void openFromPanel(const QString& file);

    NoteTreeModel model_;
    NoteTreeView tree_;
    NoteTreeDelegate treeDelegate_;
    QWidget middle_;
    QListView listView_;
    NoteListModel list_;
    NoteListDelegate listDelegate_;

    // ХОДЬБА СТРЕЛКАМИ по дереву и по списку — единственный случай, когда
    // открытая заметка НЕ забирает фокус. Отличаем не «кто в фокусе» — при
    // щелчке мышью панель тоже получает фокус, — а чем выбрали: клавишей или
    // мышью. Признак ставится до того, как панель разберёт нажатие.
    class KeyWalk;
    std::shared_ptr<KeyWalk> keyWalk_;

    // Курсор переставляем мы сами (показ открытой заметки) — обработчики выбора
    // на это время выключены: курсор здесь указатель, а не навигация.
    bool revealing_ = false;
    // Заметка, которую открыл выбор папки (первая в списке): показ её в дереве
    // курсор не двигает — человек только что ткнул в папку.
    QString openedByFolderPick_;
    QString currentNote_;
    SortOrder rootSort_ = defaultOrder(SortKey::Modified);
};

}  // namespace zametti

#endif  // ZAMETTI_NOTE_PANELS_H
