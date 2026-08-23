#include "history_panel.h"

#include "note_view.h"
#include "settings.h"

#include <QDateTime>
#include <QHBoxLayout>
#include <QLocale>
#include <QPalette>
#include <QButtonGroup>
#include <QSignalBlocker>
#include <QScrollBar>
#include <QResizeEvent>
#include <QTimer>
#include <QVBoxLayout>

namespace zametti {
namespace {

QString sizeText(qint64 bytes) {
    if (bytes < 1024) return QStringLiteral("%1 B").arg(bytes);
    if (bytes < 1024 * 1024) return QStringLiteral("%1 KB").arg(bytes / 1024);
    return QStringLiteral("%1 MB").arg(bytes / (1024 * 1024));
}

}  // namespace

QString historyMoment(qint64 msSinceEpoch) {
    const QDateTime when = QDateTime::fromMSecsSinceEpoch(msSinceEpoch).toLocalTime();
    if (!when.isValid()) return {};
    const QLocale locale;
    const QDate today = QDate::currentDate();
    if (when.date() == today)
        return QStringLiteral("today, %1").arg(locale.toString(when.time(),
                                                                QStringLiteral("HH:mm")));
    if (when.date().year() == today.year())
        return locale.toString(when, QStringLiteral("d MMMM, HH:mm"));
    return locale.toString(when, QStringLiteral("d MMMM yyyy, HH:mm"));
}

QString historyStamp(qint64 msSinceEpoch) {
    return QStringLiteral("history:") +
           QDateTime::fromMSecsSinceEpoch(msSinceEpoch)
               .toLocalTime()
               .toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
}

QString historyKindName(ZJournal::Kind kind) {
    switch (kind) {
        case ZJournal::Kind::Save: return QStringLiteral("edit");
        case ZJournal::Kind::External: return QStringLiteral("external");
        case ZJournal::Kind::Restore: return QStringLiteral("restored");
        case ZJournal::Kind::Tombstone: return QStringLiteral("deleted");
        // Гашение человеку не показывается вовсе — в список оно не попадает.
        case ZJournal::Kind::Amendment: return {};
    }
    return {};
}

HistoryBanner::HistoryBanner(QWidget* parent) : QWidget(parent) {
    const ZDocStyle& look = settings().style();
    // Баннер тонируется тем же цветом, что и поле в режиме истории: он не
    // сообщение поверх текста, а край того же прошлого.
    setStyleSheet(QStringLiteral("QWidget { background: %1; }")
                      .arg(look.historyBackground().darker(104).name()));

    text_ = new QLabel(this);
    // Надпись не диктует ширину баннера: в узком окне она ужимается первой,
    // иначе минимальная ширина баннера (и всего вида истории) отбирала бы
    // место у списка записей справа.
    text_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);

    // ПАРАМИ ЗАЛИПАЮЩИХ КНОПОК, а не одним переключателем с меняющейся
    // надписью (просьба владельца): у одной кнопки второго состояния не видно
    // вовсе, и надпись читается наоборот через раз — то как «показано сейчас»,
    // то как «будет по нажатию». Пара показывает оба ответа сразу, нажатый —
    // тот, что действует.
    const auto latching = [this](const QString& text, const QString& tip) {
        auto* button = new QPushButton(text, this);
        button->setCheckable(true);
        button->setFocusPolicy(Qt::NoFocus);   // клавиши остаются у слепка
        button->setToolTip(tip);
        return button;
    };
    fromPrevious_ = latching(QStringLiteral("vs previous"),
                             QStringLiteral("Compare with the previous history entry"));
    fromFresh_ = latching(QStringLiteral("vs current"),
                          QStringLiteral("Compare with the current version of the note"));
    // Залипают по одной: QButtonGroup держит это сам, и «оба нажаты» не
    // случится ни при какой последовательности щелчков.
    auto* baseGroup = new QButtonGroup(this);
    baseGroup->setExclusive(true);
    baseGroup->addButton(fromPrevious_);
    baseGroup->addButton(fromFresh_);
    setBaseIsFresh(false);

    leave_ = new QPushButton(QStringLiteral("To current version"), this);
    restore_ = new QPushButton(QStringLiteral("Restore this one"), this);
    restoreStyle_ = restore_->styleSheet();

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(10, 5, 10, 5);
    layout->setSpacing(8);
    layout->addWidget(text_, 1);
    layout->addWidget(new QLabel(QStringLiteral("compare:"), this));
    layout->addWidget(fromPrevious_);
    layout->addWidget(fromFresh_);
    layout->addSpacing(8);
    layout->addWidget(leave_);
    layout->addWidget(restore_);

    connect(leave_, &QPushButton::clicked, this, &HistoryBanner::leaveRequested);
    connect(restore_, &QPushButton::clicked, this, &HistoryBanner::restoreRequested);
    connect(fromFresh_, &QPushButton::clicked, this, [this] { emit baseChanged(true); });
    connect(fromPrevious_, &QPushButton::clicked, this, [this] { emit baseChanged(false); });
}

void HistoryBanner::setBaseIsFresh(bool fresh) {
    // Состояние приходит от редактора: кнопки только показывают, что действует.
    const QSignalBlocker quietFresh(fromFresh_);
    const QSignalBlocker quietPrevious(fromPrevious_);
    fromFresh_->setChecked(fresh);
    fromPrevious_->setChecked(!fresh);
}

void HistoryBanner::showText(const QString& text) {
    fullText_ = text;
    const int room = qMax(0, text_->width() - 4);
    text_->setText(room > 0 ? text_->fontMetrics().elidedText(text, Qt::ElideRight, room) : text);
    text_->setToolTip(text);
}

void HistoryBanner::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    if (!fullText_.isEmpty()) showText(fullText_);
}

void HistoryBanner::setSnapshot(qint64 time, ZJournal::Kind kind, int changed) {
    restore_->setStyleSheet(restoreStyle_);
    // Строка отвечает на ОДИН вопрос: какая версия сейчас перед глазами
    // (просьба владельца; всё, что было после тире, убрано). Счёт тронутых
    // строк — тихой добавкой: сколько разница весит, видно до прокрутки.
    QString what = QStringLiteral("Snapshot from %1").arg(historyMoment(time));
    if (kind != ZJournal::Kind::Save)
        what += QStringLiteral(" (%1)").arg(historyKindName(kind));
    if (changed >= 0) what += QStringLiteral("  ·  ±%1").arg(changed);
    showText(what);
}

void HistoryBanner::flashRestore() {
    // Подсветка вместо действия: печатающая клавиша ничего не восстанавливает,
    // но и молчать в ответ нельзя — человек нажал не просто так.
    showText(QStringLiteral("The snapshot is read-only. "
                            "To bring its content back — “Restore this one”."));
    restore_->setStyleSheet(QStringLiteral("QPushButton { border: 2px solid %1; }")
                                .arg(settings().style().caretColor().name()));
    QTimer::singleShot(1200, this, [this] { restore_->setStyleSheet(restoreStyle_); });
}

HistoryTimeline::HistoryTimeline(QWidget* parent) : QWidget(parent) {
    auto* title = new QLabel(QStringLiteral("History"), this);
    auto* close = new QPushButton(QStringLiteral("×"), this);
    close->setFixedWidth(24);
    close->setToolTip(QStringLiteral("Close history and return to the current version"));
    list_ = new QListWidget(this);
    list_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    // Показанная запись обязана быть видна и когда фокус в тексте — а туда он
    // уходит почти всегда: человек ходит по истории с клавиатуры и выделяет
    // куски. По умолчанию Qt рисует выделение неактивного списка почти
    // неразличимо; задаём цвет обеим группам.
    QPalette listPalette = list_->palette();
    for (QPalette::ColorGroup group : {QPalette::Active, QPalette::Inactive}) {
        listPalette.setColor(group, QPalette::Highlight, settings().style().selectionBackground());
        listPalette.setColor(group, QPalette::HighlightedText,
                             selectedTextColour(settings().style(), listPalette));
    }
    list_->setPalette(listPalette);

    auto* head = new QHBoxLayout;
    head->setContentsMargins(6, 4, 4, 4);
    head->addWidget(title, 1);
    head->addWidget(close);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addLayout(head);
    layout->addWidget(list_, 1);

    connect(close, &QPushButton::clicked, this, &HistoryTimeline::closeRequested);
    connect(list_, &QListWidget::currentRowChanged, this, [this](int row) {
        if (quiet_ || row < 0 || row >= entries_.size()) return;
        emit entryChosen(list_->item(row)->data(Qt::UserRole).toInt());
    });
}

void HistoryTimeline::setEntries(const QVector<ZJournal::Entry>& entries) {
    entries_ = entries;
    quiet_ = true;
    list_->clear();
    // Свежие сверху — как в списке заметок: в прошлое человек идёт сверху вниз.
    for (int i = entries.size() - 1; i >= 0; --i) {
        const ZJournal::Entry& entry = entries[i];
        QString line = historyMoment(entry.time());
        if (entry.kind() != ZJournal::Kind::Save)
            line += QStringLiteral("  ·  %1").arg(historyKindName(entry.kind()));
        if (entry.hasSnapshot()) line += QStringLiteral("  ·  %1").arg(sizeText(entry.plainSize()));
        auto* item = new QListWidgetItem(line, list_);
        item->setData(Qt::UserRole, i);
        // У надгробия смотреть нечего: заметка удалена, слепок — предыдущая
        // запись. Строку показываем (иначе история врала бы умолчанием), но
        // выбрать её нельзя.
        if (!entry.hasSnapshot()) item->setFlags(item->flags() & ~Qt::ItemIsSelectable);
    }
    quiet_ = false;
}

int HistoryTimeline::contentWidth() const {
    return list_->sizeHintForColumn(0) + list_->verticalScrollBar()->sizeHint().width() +
           2 * list_->frameWidth() + 8;
}

void HistoryTimeline::setCurrent(int index) {
    quiet_ = true;
    for (int row = 0; row < list_->count(); ++row) {
        if (list_->item(row)->data(Qt::UserRole).toInt() != index) continue;
        list_->setCurrentRow(row);
        break;
    }
    quiet_ = false;
}

}  // namespace zametti
