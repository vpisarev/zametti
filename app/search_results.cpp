#include "search_results.h"

#include "settings.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QFontMetrics>
#include <QPainter>

namespace zametti {

namespace {

constexpr qreal kPaddingFactor = 0.25;
constexpr int kIndentChars = 2;

}  // namespace

void SearchResultsModel::setResults(const QVector<SearchResult>& results) {
    beginResetModel();
    rows_.clear();
    QString lastNote;
    for (const SearchResult& result : results) {
        if (result.noteId != lastNote) {
            lastNote = result.noteId;
            Row header;
            header.header = true;
            // АРХИВНАЯ ЗАМЕТКА НАЗЫВАЕТСЯ ТАКОЙ ПРЯМО В ЗАГОЛОВКЕ ГРУППЫ.
            // Иначе человек, нашедший убранное, узнал бы об этом только
            // открыв находку — и удивился бы серому полю без каретки.
            header.title = result.archived
                               ? result.title + QStringLiteral("  ·  archived")
                               : result.title;
            header.path = result.path;
            header.snapshotTime = result.snapshotTime;
            rows_.push_back(header);
        }
        Row row;
        row.line = result.line;
        row.offset = result.lineOffset;
        row.length = result.lineLength;
        row.path = result.path;
        row.ordinal = result.ordinal;
        row.snapshotTime = result.snapshotTime;
        row.snapshotDigest = result.snapshotDigest;
        rows_.push_back(row);
    }
    endResetModel();
}

void SearchResultsModel::clear() {
    beginResetModel();
    rows_.clear();
    endResetModel();
}

int SearchResultsModel::rowCount(const QModelIndex& parent) const {
    if (parent.isValid()) return 0;
    return int(rows_.size());
}

QVariant SearchResultsModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= int(rows_.size())) return {};
    const Row& row = rows_[size_t(index.row())];
    switch (role) {
        case Qt::DisplayRole: return row.header ? row.title : row.line;
        case HeaderRole: return row.header;
        case LineRole: return row.line;
        case OffsetRole: return row.offset;
        case LengthRole: return row.length;
        case PathRole: return row.path;
        case OrdinalRole: return row.ordinal;
        case SnapshotTimeRole: return row.snapshotTime;
        case SnapshotDigestRole:
            return QByteArray(reinterpret_cast<const char*>(row.snapshotDigest.bytes.data()),
                              qsizetype(row.snapshotDigest.bytes.size()));
        default: return {};
    }
}

Qt::ItemFlags SearchResultsModel::flags(const QModelIndex& index) const {
    Qt::ItemFlags out = QAbstractListModel::flags(index);
    // Заголовок — не цель: щёлкать по нему нечего, совпадения ниже.
    if (index.isValid() && rows_[size_t(index.row())].header)
        out &= ~(Qt::ItemIsSelectable | Qt::ItemIsEnabled);
    return out;
}

QModelIndex SearchResultsModel::firstHit(int from, int direction) const {
    if (rows_.empty()) return {};
    const int count = int(rows_.size());
    for (int step = 0; step < count; ++step) {
        int at = from + direction * step;
        at = ((at % count) + count) % count;
        if (!rows_[size_t(at)].header) return index(at, 0);
    }
    return {};
}

bool SearchResultsModel::isHeader(const QModelIndex& index) const {
    return index.isValid() && index.row() < int(rows_.size()) &&
           rows_[size_t(index.row())].header;
}

void SearchResultsDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option,
                                  const QModelIndex& index) const {
    painter->save();

    QStyleOptionViewItem opt = option;
    initStyleOption(&opt, index);
    opt.text.clear();
    QStyle* style = opt.widget != nullptr ? opt.widget->style() : QApplication::style();
    style->drawControl(QStyle::CE_ItemViewItem, &opt, painter, opt.widget);

    const bool header = index.data(SearchResultsModel::HeaderRole).toBool();
    const bool selected = (option.state & QStyle::State_Selected) != 0;
    QFont font = option.font;
    if (header) font.setBold(true);
    const QFontMetrics metrics(font);
    painter->setFont(font);

    const int padding = int(metrics.height() * kPaddingFactor);
    const int indent = header ? padding : padding + metrics.horizontalAdvance(QLatin1Char(' ')) *
                                                        kIndentChars;
    QRect body = option.rect.adjusted(indent, padding, -padding, -padding);

    const QColor text = selected ? option.palette.color(QPalette::HighlightedText)
                                 : option.palette.color(QPalette::Text);
    painter->setPen(text);

    if (header) {
        painter->drawText(body, Qt::AlignLeft | Qt::AlignVCenter,
                          metrics.elidedText(index.data(Qt::DisplayRole).toString(),
                                             Qt::ElideRight, body.width()));
        painter->restore();
        return;
    }

    // Строка совпадения: до, само совпадение тонировкой, после. Резать по
    // ширине приходится вручную — иначе подсветка съедет вместе с многоточием.
    const QString line = index.data(SearchResultsModel::LineRole).toString();
    const int offset = qBound(0, index.data(SearchResultsModel::OffsetRole).toInt(),
                              int(line.size()));
    const int length =
        qBound(0, index.data(SearchResultsModel::LengthRole).toInt(), int(line.size()) - offset);

    const QString before = line.left(offset);
    const QString hit = line.mid(offset, length);
    const QString after = line.mid(offset + length);

    int x = body.left();
    const int beforeWidth = metrics.horizontalAdvance(before);
    painter->drawText(QRect(x, body.top(), beforeWidth, body.height()),
                      Qt::AlignLeft | Qt::AlignVCenter, before);
    x += beforeWidth;

    const int hitWidth = metrics.horizontalAdvance(hit);
    const QRect hitRect(x, body.top(), hitWidth, body.height());
    // Тем же цветом, что и находки в самой заметке: перешёл по строке — и
    // увидел на прежнем месте то же самое пятно, только в тексте.
    painter->fillRect(hitRect.adjusted(0, 1, 0, -1), settings().style().searchHighlight());
    painter->setPen(option.palette.color(QPalette::Text));
    painter->drawText(hitRect, Qt::AlignLeft | Qt::AlignVCenter, hit);
    x += hitWidth;

    painter->setPen(text);
    const int rest = qMax(0, body.right() - x);
    painter->drawText(QRect(x, body.top(), rest, body.height()),
                      Qt::AlignLeft | Qt::AlignVCenter,
                      metrics.elidedText(after, Qt::ElideRight, rest));

    painter->restore();
}

QSize SearchResultsDelegate::sizeHint(const QStyleOptionViewItem& option,
                                      const QModelIndex& index) const {
    QFont font = option.font;
    if (index.data(SearchResultsModel::HeaderRole).toBool()) font.setBold(true);
    const QFontMetrics metrics(font);
    int width = option.rect.width();
    if (const auto* view = qobject_cast<const QAbstractItemView*>(option.widget))
        width = view->viewport()->width();
    return {width, metrics.height() + int(metrics.height() * kPaddingFactor) * 2};
}

}  // namespace zametti
