// Список результатов поиска по всему хранилищу.
//
// Строки двух родов: заголовок заметки и совпадение под ним. Совпадений у
// одной заметки может быть несколько — каждое отдельной строкой, но заголовок
// показан один раз: иначе список превращается в столбец из одного и того же
// названия.

#ifndef ZAMETTI_SEARCH_RESULTS_H
#define ZAMETTI_SEARCH_RESULTS_H

#include "store_search.h"

#include <QAbstractListModel>
#include <QStyledItemDelegate>

#include <vector>

namespace zametti {

class SearchResultsModel : public QAbstractListModel {
    Q_OBJECT

public:
    struct Row {
        bool header = false;
        QString title;
        QString line;
        int offset = 0;
        int length = 0;
        QString path;
        int ordinal = 0;
    };

    enum Roles {
        HeaderRole = Qt::UserRole + 1,
        LineRole,
        OffsetRole,
        LengthRole,
        PathRole,
        OrdinalRole,
    };

    using QAbstractListModel::QAbstractListModel;

    void setResults(const QVector<SearchResult>& results);
    void clear();

    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    Qt::ItemFlags flags(const QModelIndex& index) const override;

    // Первая строка-совпадение, начиная с данной (заголовки пропускаются):
    // ими ходят F3 и Shift+F3.
    QModelIndex firstHit(int from, int direction) const;
    bool isHeader(const QModelIndex& index) const;

private:
    std::vector<Row> rows_;
};

// Заголовок — жирным, совпадение внутри строки — тонировкой: без неё в
// длинной строке не видно, за что зацепился поиск.
class SearchResultsDelegate : public QStyledItemDelegate {
    Q_OBJECT

public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override;
    QSize sizeHint(const QStyleOptionViewItem& option,
                   const QModelIndex& index) const override;
};

}  // namespace zametti

#endif  // ZAMETTI_SEARCH_RESULTS_H
