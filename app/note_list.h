// Средняя колонка: плоский список заметок выбранного поддерева.
//
// Плоский — значит без группировки по подпапкам: выбрана папка, видны все
// заметки под ней, включая под-под-папки. Так устроен Apple Notes, и так
// заметка не прячется в ветке, о которой человек забыл.
//
// Модель ничего не сканирует сама: строки ей даёт дерево (оно уже прочитало
// хранилище — заголовок, начало текста, дату). Второй проход по файлам ради
// той же информации был бы чистой тратой (замер этапа 4: 7 мс на проход).

#ifndef ZAMETTI_NOTE_LIST_H
#define ZAMETTI_NOTE_LIST_H

#include "note_tree.h"

#include <QAbstractListModel>
#include <QCollator>
#include <QListView>
#include <QStyledItemDelegate>

#include <vector>

namespace zametti {

class NoteListModel : public QAbstractListModel {
    Q_OBJECT

public:
    // Роли сверх Display: делегат рисует три вещи разом, а склеивать их в одну
    // строку нельзя — у каждой свой шрифт и цвет.
    enum Roles {
        TitleRole = Qt::UserRole + 1,
        SnippetRole,
        DateRole,
        PathRole,
        IdRole,
    };

    explicit NoteListModel(QObject* parent = nullptr);

    // Сортировка общая с деревом: переключатель один на обе панели.
    void setSortOrder(SortOrder order);
    SortOrder sortOrder() const { return sortOrder_; }

    // ПОРЯДОК ЗАДАН СНАРУЖИ. У виртуальной папки (документация) дат нет вовсе,
    // а порядок есть: тот, в котором её завели. Сортировать такие строки по
    // пустым датам значит показывать их вперемешку и по-разному от запуска к
    // запуску. Ставится перед setRows и держится до следующей смены папки.
    void setFixedOrder(bool on) { fixedOrder_ = on; }
    bool fixedOrder() const { return fixedOrder_; }

    // Полная замена содержимого: сменилась выбранная папка или перестроилось
    // дерево.
    void setRows(std::vector<NoteRow> rows);

    // Обновить одну строку (заголовок, сниппет, дата) — после сохранения
    // заметки. Позиция в сортировке при этом может поменяться, поэтому строки
    // пересортировываются: заметка, которую только что правили, обязана
    // всплыть наверх в режиме «по дате».
    void updateRow(const NoteRow& row);

    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;

    // Перетаскивание строки на папку левой панели = перенос. Формат тот же,
    // что у дерева: принимающая сторона одна, и знать про две записи id ей
    // незачем.
    Qt::ItemFlags flags(const QModelIndex& index) const override;
    QStringList mimeTypes() const override;
    QMimeData* mimeData(const QModelIndexList& indexes) const override;

    QString pathAt(const QModelIndex& index) const;
    QString idAt(const QModelIndex& index) const;
    QModelIndex indexForPath(const QString& path) const;

private:
    void sortRows();

    std::vector<NoteRow> rows_;
    SortOrder sortOrder_ = defaultOrder(SortKey::Modified);
    bool fixedOrder_ = false;
    QCollator collator_;
};

// Строка: заголовок, под ним пара строк начала текста, справа от заголовка —
// дата правки. Рисуется вручную: штатный делегат умеет ровно одну строку
// текста, а тут их три и у каждой свой цвет.
class NoteListDelegate : public QStyledItemDelegate {
    Q_OBJECT

public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override;
    QSize sizeHint(const QStyleOptionViewItem& option,
                   const QModelIndex& index) const override;
};

// Дата в короткой записи: сегодня — время, в этом году — «14 мар», раньше —
// «14.03.2019». В списке важна не точность, а узнаваемость.
QString shortDate(const QString& isoModified);

}  // namespace zametti

#endif  // ZAMETTI_NOTE_LIST_H
