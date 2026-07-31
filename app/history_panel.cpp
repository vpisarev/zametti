#include "history_panel.h"

#include "settings.h"

#include <QDateTime>
#include <QHBoxLayout>
#include <QLocale>
#include <QPalette>
#include <QTimer>
#include <QVBoxLayout>

namespace zametti {
namespace {

QString sizeText(qint64 bytes) {
    if (bytes < 1024) return QStringLiteral("%1 Б").arg(bytes);
    if (bytes < 1024 * 1024) return QStringLiteral("%1 КБ").arg(bytes / 1024);
    return QStringLiteral("%1 МБ").arg(bytes / (1024 * 1024));
}

}  // namespace

QString historyMoment(qint64 msSinceEpoch) {
    const QDateTime when = QDateTime::fromMSecsSinceEpoch(msSinceEpoch).toLocalTime();
    if (!when.isValid()) return {};
    const QLocale locale;
    const QDate today = QDate::currentDate();
    if (when.date() == today)
        return QStringLiteral("сегодня, %1").arg(locale.toString(when.time(),
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

QString historyKindName(journal::Kind kind) {
    switch (kind) {
        case journal::Kind::Save: return QStringLiteral("правка");
        case journal::Kind::External: return QStringLiteral("извне");
        case journal::Kind::Restore: return QStringLiteral("восстановлено");
        case journal::Kind::Tombstone: return QStringLiteral("удалена");
    }
    return {};
}

HistoryBanner::HistoryBanner(QWidget* parent) : QWidget(parent) {
    const Appearance& look = appearance();
    // Баннер тонируется тем же цветом, что и поле в режиме истории: он не
    // сообщение поверх текста, а край того же прошлого.
    setStyleSheet(QStringLiteral("QWidget { background: %1; }")
                      .arg(look.historyBackground.darker(104).name()));

    text_ = new QLabel(this);
    leave_ = new QPushButton(QStringLiteral("К текущей версии"), this);
    restore_ = new QPushButton(QStringLiteral("Восстановить эту"), this);
    restoreStyle_ = restore_->styleSheet();

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(10, 5, 10, 5);
    layout->setSpacing(8);
    layout->addWidget(text_, 1);
    layout->addWidget(leave_);
    layout->addWidget(restore_);

    connect(leave_, &QPushButton::clicked, this, &HistoryBanner::leaveRequested);
    connect(restore_, &QPushButton::clicked, this, &HistoryBanner::restoreRequested);
}

void HistoryBanner::setSnapshot(qint64 time, journal::Kind kind) {
    restore_->setStyleSheet(restoreStyle_);
    QString what = QStringLiteral("Слепок от %1").arg(historyMoment(time));
    if (kind != journal::Kind::Save)
        what += QStringLiteral(" (%1)").arg(historyKindName(kind));
    text_->setText(what + QStringLiteral(" — только чтение"));
}

void HistoryBanner::flashRestore() {
    // Подсветка вместо действия: печатающая клавиша ничего не восстанавливает,
    // но и молчать в ответ нельзя — человек нажал не просто так.
    text_->setText(QStringLiteral("Слепок только для чтения. "
                                  "Чтобы вернуть его содержимое — «Восстановить эту»."));
    restore_->setStyleSheet(QStringLiteral("QPushButton { border: 2px solid %1; }")
                                .arg(appearance().caretColor.name()));
    QTimer::singleShot(1200, this, [this] { restore_->setStyleSheet(restoreStyle_); });
}

HistoryTimeline::HistoryTimeline(QWidget* parent) : QWidget(parent) {
    auto* title = new QLabel(QStringLiteral("История"), this);
    auto* close = new QPushButton(QStringLiteral("×"), this);
    close->setFixedWidth(24);
    close->setToolTip(QStringLiteral("Закрыть историю и вернуться к текущей версии"));
    list_ = new QListWidget(this);
    list_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    // Показанная запись обязана быть видна и когда фокус в тексте — а туда он
    // уходит почти всегда: человек ходит по истории с клавиатуры и выделяет
    // куски. По умолчанию Qt рисует выделение неактивного списка почти
    // неразличимо; задаём цвет обеим группам.
    QPalette listPalette = list_->palette();
    for (QPalette::ColorGroup group : {QPalette::Active, QPalette::Inactive}) {
        listPalette.setColor(group, QPalette::Highlight, appearance().selectionBackground);
        listPalette.setColor(group, QPalette::HighlightedText,
                             listPalette.color(QPalette::Text));
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

void HistoryTimeline::setEntries(const QVector<journal::Entry>& entries) {
    entries_ = entries;
    quiet_ = true;
    list_->clear();
    // Свежие сверху — как в списке заметок: в прошлое человек идёт сверху вниз.
    for (int i = entries.size() - 1; i >= 0; --i) {
        const journal::Entry& entry = entries[i];
        QString line = historyMoment(entry.time);
        if (entry.kind != journal::Kind::Save)
            line += QStringLiteral("  ·  %1").arg(historyKindName(entry.kind));
        if (entry.hasSnapshot()) line += QStringLiteral("  ·  %1").arg(sizeText(entry.plainSize));
        auto* item = new QListWidgetItem(line, list_);
        item->setData(Qt::UserRole, i);
        // У надгробия смотреть нечего: заметка удалена, слепок — предыдущая
        // запись. Строку показываем (иначе история врала бы умолчанием), но
        // выбрать её нельзя.
        if (!entry.hasSnapshot()) item->setFlags(item->flags() & ~Qt::ItemIsSelectable);
    }
    quiet_ = false;
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
