#include "note_list.h"

#include "settings.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QDateTime>
#include <QFontMetrics>
#include <QLocale>
#include <QMimeData>
#include <QPainter>
#include <QTextOption>

#include <algorithm>

namespace zametti {

namespace {

// Зазоры строки списка, в долях от высоты шрифта: пиксели не пережили бы смену
// кегля панели.
constexpr qreal kPaddingFactor = 0.45;
constexpr qreal kGapFactor = 0.15;

QDateTime whenOf(const QString& iso) {
    return QDateTime::fromString(iso, Qt::ISODate).toLocalTime();
}

}  // namespace

QString shortDate(const QString& isoModified) {
    const QDateTime when = whenOf(isoModified);
    if (!when.isValid()) return {};
    const QDate today = QDate::currentDate();
    const QDate day = when.date();
    const QLocale locale;
    if (day == today) return locale.toString(when.time(), QStringLiteral("HH:mm"));
    if (day.year() == today.year())
        return locale.toString(day, QStringLiteral("d MMM"));
    return locale.toString(day, QStringLiteral("dd.MM.yyyy"));
}

NoteListModel::NoteListModel(QObject* parent) : QAbstractListModel(parent) {
    collator_.setNumericMode(true);
    collator_.setCaseSensitivity(Qt::CaseInsensitive);
}

void NoteListModel::setSortMode(NoteTreeModel::SortMode mode) {
    if (mode == sortMode_) return;
    sortMode_ = mode;
    beginResetModel();
    sortRows();
    endResetModel();
}

void NoteListModel::sortRows() {
    std::sort(rows_.begin(), rows_.end(), [this](const NoteRow& a, const NoteRow& b) {
        if (sortMode_ == NoteTreeModel::SortMode::ByName)
            return collator_.compare(a.title, b.title) < 0;
        // Свежие сверху. Записи ISO сравниваются как строки: они одной длины и
        // всегда в UTC — так их пишет ядро.
        if (a.modified != b.modified) return a.modified > b.modified;
        return collator_.compare(a.title, b.title) < 0;
    });
}

void NoteListModel::setRows(std::vector<NoteRow> rows) {
    beginResetModel();
    rows_ = std::move(rows);
    sortRows();
    endResetModel();
}

void NoteListModel::updateRow(const NoteRow& row) {
    for (size_t i = 0; i < rows_.size(); ++i) {
        if (rows_[i].id != row.id) continue;
        const bool moves = sortMode_ == NoteTreeModel::SortMode::ByModified
                               ? rows_[i].modified != row.modified
                               : rows_[i].title != row.title;
        rows_[i] = row;
        if (!moves) {
            const QModelIndex at = index(int(i), 0);
            emit dataChanged(at, at);
            return;
        }
        // Порядок поехал — проще пересобрать целиком: строк сотни, а
        // выцеливать перемещение одной ради экономии микросекунд значит
        // заводить второй способ упорядочивать список.
        beginResetModel();
        sortRows();
        endResetModel();
        return;
    }
}

int NoteListModel::rowCount(const QModelIndex& parent) const {
    if (parent.isValid()) return 0;
    return int(rows_.size());
}

QVariant NoteListModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= int(rows_.size())) return {};
    const NoteRow& row = rows_[size_t(index.row())];
    switch (role) {
        case Qt::DisplayRole:
        case TitleRole: return row.title;
        case SnippetRole: return row.snippet;
        case DateRole: return shortDate(row.modified);
        case PathRole: return row.path;
        case IdRole: return row.id;
        case Qt::ToolTipRole: return row.title;
        default: return {};
    }
}

Qt::ItemFlags NoteListModel::flags(const QModelIndex& index) const {
    Qt::ItemFlags out = QAbstractListModel::flags(index);
    if (index.isValid()) out |= Qt::ItemIsDragEnabled;
    return out;
}

QStringList NoteListModel::mimeTypes() const {
    return {QStringLiteral("application/x-zametti-note-id")};
}

QMimeData* NoteListModel::mimeData(const QModelIndexList& indexes) const {
    if (indexes.isEmpty()) return nullptr;
    auto* data = new QMimeData;
    data->setData(QStringLiteral("application/x-zametti-note-id"),
                  idAt(indexes.first()).toUtf8());
    return data;
}

QString NoteListModel::pathAt(const QModelIndex& index) const {
    return data(index, PathRole).toString();
}

QString NoteListModel::idAt(const QModelIndex& index) const {
    return data(index, IdRole).toString();
}

QModelIndex NoteListModel::indexForPath(const QString& path) const {
    for (size_t i = 0; i < rows_.size(); ++i)
        if (rows_[i].path == path) return index(int(i), 0);
    return {};
}

void NoteListDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option,
                             const QModelIndex& index) const {
    painter->save();

    // Фон и рамку выделения рисует стиль, текст — мы. Звать
    // QStyledItemDelegate::paint нельзя: он заново берёт подпись из модели и
    // кладёт её поверх нашей (замерено — заголовок двоился).
    QStyleOptionViewItem opt = option;
    initStyleOption(&opt, index);
    opt.text.clear();
    QStyle* style = opt.widget != nullptr ? opt.widget->style() : QApplication::style();
    style->drawControl(QStyle::CE_ItemViewItem, &opt, painter, opt.widget);

    const Appearance& a = appearance();
    const QFontMetrics metrics(option.font);
    const int padding = int(metrics.height() * kPaddingFactor);
    const int gap = int(metrics.height() * kGapFactor);
    const bool selected = (option.state & QStyle::State_Selected) != 0;

    const QRect body = option.rect.adjusted(padding, padding, -padding, -padding);

    QFont titleFont = option.font;
    titleFont.setBold(true);
    const QFontMetrics titleMetrics(titleFont);

    // Дата держит своё место справа, заголовок укорачивается до остатка: иначе
    // длинное название наезжает на дату.
    const QString date = index.data(NoteListModel::DateRole).toString();
    const int dateWidth = date.isEmpty() ? 0 : metrics.horizontalAdvance(date);
    const int titleWidth = qMax(0, body.width() - dateWidth - (dateWidth > 0 ? gap * 2 : 0));
    const QRect titleRect(body.left(), body.top(), titleWidth, titleMetrics.height());

    if (dateWidth > 0) {
        painter->setFont(option.font);
        painter->setPen(selected ? option.palette.color(QPalette::HighlightedText)
                                 : a.noteListDateColor);
        painter->drawText(QRect(body.right() - dateWidth, body.top(), dateWidth,
                                titleMetrics.height()),
                          Qt::AlignRight | Qt::AlignVCenter, date);
    }

    painter->setFont(titleFont);
    painter->setPen(selected ? option.palette.color(QPalette::HighlightedText)
                             : option.palette.color(QPalette::Text));
    painter->drawText(titleRect, Qt::AlignLeft | Qt::AlignVCenter,
                      titleMetrics.elidedText(index.data(NoteListModel::TitleRole).toString(),
                                              Qt::ElideRight, titleRect.width()));

    const int lines = a.noteListSnippetLines;
    const QString snippet = index.data(NoteListModel::SnippetRole).toString();
    if (lines > 0 && !snippet.isEmpty()) {
        painter->setFont(option.font);
        painter->setPen(selected ? option.palette.color(QPalette::HighlightedText)
                                 : a.noteListSnippetColor);
        const QRect snippetRect(body.left(), titleRect.bottom() + gap, body.width(),
                                metrics.lineSpacing() * lines);
        QTextOption wrap(Qt::AlignLeft | Qt::AlignTop);
        wrap.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
        // Многоточие ставим сами: QPainter::drawText с переносом умеет обрезать
        // текст по рамке, но не отмечает, что дальше есть ещё.
        QString shown = snippet;
        const int capacity = int(snippetRect.width() * lines / qMax(1, metrics.averageCharWidth()));
        if (shown.size() > capacity) shown = shown.left(qMax(0, capacity - 1)) + QChar(0x2026);
        painter->drawText(snippetRect, shown, wrap);
    }

    painter->restore();
}

QSize NoteListDelegate::sizeHint(const QStyleOptionViewItem& option,
                                 const QModelIndex& index) const {
    // Ширину берём у самого представления: option.rect при опросе размера
    // может быть ещё пустым, а вернуть больше вьюпорта — значит завести
    // горизонтальную прокрутку, которая срежет дату у правого края.
    int width = option.rect.width();
    if (const auto* view = qobject_cast<const QAbstractItemView*>(option.widget))
        width = view->viewport()->width();
    const QFontMetrics metrics(option.font);
    const int padding = int(metrics.height() * kPaddingFactor);
    const int gap = int(metrics.height() * kGapFactor);
    const int lines = appearance().noteListSnippetLines;
    const bool hasSnippet =
        lines > 0 && !index.data(NoteListModel::SnippetRole).toString().isEmpty();
    QFont titleFont = option.font;
    titleFont.setBold(true);
    int height = padding * 2 + QFontMetrics(titleFont).height();
    if (hasSnippet) height += gap + metrics.lineSpacing() * lines;
    return {width, height};
}

}  // namespace zametti
