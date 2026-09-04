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
#include <QPainter>
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

ElidingLabel::ElidingLabel(QWidget* parent) : QLabel(parent) {
    // Просит столько, сколько нужно тексту, но отдаёт место первой: политика
    // Preferred, а минимум — через minimumSizeHint ниже.
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
}

QSize ElidingLabel::sizeHint() const {
    const QFontMetrics metrics(font());
    return QSize(metrics.horizontalAdvance(text()) + 2, metrics.height());
}

QSize ElidingLabel::minimumSizeHint() const {
    const QFontMetrics metrics(font());
    return QSize(metrics.horizontalAdvance(QStringLiteral("…")), metrics.height());
}

void ElidingLabel::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event);
    QPainter painter(this);
    painter.setFont(font());
    painter.setPen(palette().color(QPalette::WindowText));
    const QString shown = fontMetrics().elidedText(text(), Qt::ElideRight, width());
    painter.drawText(rect(), int(alignment()) | Qt::TextSingleLine, shown);
}

HistoryBanner::HistoryBanner(QWidget* parent) : QWidget(parent) {
    const ZDocStyle& look = settings().style();
    // Баннер тонируется тем же цветом, что и поле в режиме истории: он не
    // сообщение поверх текста, а край того же прошлого. ПАЛИТРОЙ, А НЕ
    // STYLESHEET (как тулбар и полоса сведений): у виджета со stylesheet Qt
    // отключает наследование шрифта детьми, и шрифт оболочки, поставленный
    // баннеру, до надписей и кнопок не доходил — на маке они оставались на
    // системном шрифте (снимок владельца, 04.09.2026; набор HistoryView).
    setAutoFillBackground(true);
    QPalette tint = palette();
    tint.setColor(QPalette::Window, look.historyBackground().darker(104));
    setPalette(tint);

    text_ = new ElidingLabel(this);
    text_->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    // Сколько строк добавлено и убрано — «+m/−n» СРАЗУ ЗА строкой слепка, своей
    // надписью: хвостом первой оно резалось многоточием первым (снимок
    // владельца, 04.09.2026: «Snapshot from today, 11:58…» и ни одного числа).
    // +m на фоне diff.added, −n — diff.removed: те же цвета, что у строк на
    // поле, и числа читаются без легенды (просьба владельца).
    counts_ = new QLabel(this);
    counts_->setTextFormat(Qt::RichText);
    counts_->setToolTip(QStringLiteral("Lines added / removed against the base"));
    // Счёт отличий — у ПРАВОГО края строки слепка: слева «Date …», справа
    // «3/12». Своей надписью, а не хвостом первой: та ужимается многоточием в
    // узком окне, и счёт исчезал бы первым — а он короткий и нужен всегда.
    hunks_ = new QLabel(this);
    hunks_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    hunks_->setToolTip(QStringLiteral("Difference under the caret / how many there are"));
    countsDot_ = new QLabel(QStringLiteral("·"), this);
    hunksDot_ = new QLabel(QStringLiteral("·"), this);
    countsDot_->setAlignment(Qt::AlignCenter);
    hunksDot_->setAlignment(Qt::AlignCenter);
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
    // ПОДПИСЬ ЦЕЛИКОМ НА КНОПКЕ, отдельного слова «compare:» перед ними нет
    // (просьба владельца): оно стояло вплотную к счёту отличий, и две короткие
    // надписи рядом читались как одна. Кнопка обязана говорить сама за себя —
    // тем более что рядом с ней теперь стоит число.
    fromPrevious_ = latching(QStringLiteral("diff vs previous"),
                             QStringLiteral("Compare with the previous history entry"));
    fromFresh_ = latching(QStringLiteral("diff vs current"),
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
    // Секции строки стоят плотно, через «·» (просьба владельца: лишние
    // пробелы перед счётом отнимали место у даты); воздух — только между
    // строкой и кнопками, ниже.
    layout->setSpacing(6);
    layout->addWidget(text_);
    layout->addWidget(countsDot_);
    layout->addWidget(counts_);
    layout->addWidget(hunksDot_);
    layout->addWidget(hunks_);
    // Свободное место — МЕЖДУ строкой и кнопками, а не внутри строки: секции
    // стоят плотно у даты, а в узком окне первой ужимается дата (её надпись
    // просит место по полному тексту, но отдаёт до многоточия).
    layout->addStretch(1);
    layout->addSpacing(8);
    layout->addWidget(fromPrevious_);
    layout->addWidget(fromFresh_);
    layout->addSpacing(8);
    layout->addWidget(leave_);
    layout->addWidget(restore_);

    syncDots();

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
    // Полный текст — в подсказке: обрезанный конец читается там.
    text_->setText(text);
    text_->setToolTip(text);
}

void HistoryBanner::setSnapshot(qint64 time, ZJournal::Kind kind, int added, int removed) {
    restore_->setStyleSheet(restoreStyle_);
    // Строка отвечает на ОДИН вопрос: какая версия сейчас перед глазами
    // (просьба владельца; всё, что было после тире, убрано). «Date:», а не
    // «Snapshot from»: короче, и дата не режется первой в узком окне.
    QString what = QStringLiteral("Date: %1").arg(historyMoment(time));
    if (kind != ZJournal::Kind::Save)
        what += QStringLiteral(" (%1)").arg(historyKindName(kind));
    showText(what);
    // Сколько разница весит — видно до прокрутки: «+m/−n», как в git; числа на
    // фоне своих цветов разности.
    if (added < 0 || removed < 0) {
        counts_->clear();
    } else {
        // Тот же вид, что у строк на поле: цвет разности С ПРОЗРАЧНОСТЬЮ
        // diffTint поверх фона (сырой diff.added — густо-зелёный, а строки
        // залиты бледным). Rich text прозрачность не берёт — смешиваем с фоном
        // баннера сами.
        const ZDocStyle& look = settings().style();
        const QColor ground = palette().color(QPalette::Window);
        const auto tinted = [&](QColor colour) {
            const qreal a = qBound(0, look.diffTint(), 255) / 255.0;
            return QColor(int(ground.red() * (1 - a) + colour.red() * a),
                          int(ground.green() * (1 - a) + colour.green() * a),
                          int(ground.blue() * (1 - a) + colour.blue() * a));
        };
        counts_->setText(QStringLiteral("<span style=\"background-color:%1\">&nbsp;+%2&nbsp;</span>"
                                        "/"
                                        "<span style=\"background-color:%3\">&nbsp;−%4&nbsp;</span>")
                             .arg(tinted(look.diffAdded()).name())
                             .arg(added)
                             .arg(tinted(look.diffRemoved()).name())
                             .arg(removed));
    }
    syncDots();
}

void HistoryBanner::setHunk(int index, int total) {
    if (total <= 0) {
        hunks_->clear();
    } else {
        // «—/12», пока ни на одном: ноль читался бы как «нулевое отличие».
        hunks_->setText(index > 0 ? QStringLiteral("%1/%2").arg(index).arg(total)
                                  : QStringLiteral("—/%1").arg(total));
    }
    syncDots();
}

void HistoryBanner::syncDots() {
    // Точка стоит перед секцией и гаснет вместе с ней: «Date: … · +1/−5 · —/3»,
    // а без отличий — просто «Date: …».
    countsDot_->setVisible(!counts_->text().isEmpty());
    hunksDot_->setVisible(!hunks_->text().isEmpty());
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

void HistoryTimeline::setEntries(const ZJournal& journal) {
    const QVector<ZJournal::Record>& entries = journal.entries();
    entries_ = entries;
    quiet_ = true;
    list_->clear();
    // Свежие сверху — как в списке заметок: в прошлое человек идёт сверху вниз.
    for (int i = entries.size() - 1; i >= 0; --i) {
        const ZJournal::Record& entry = entries[i];
        // ГАШЕНИЕ ЧЕЛОВЕКУ НЕ ПОКАЗЫВАЕТСЯ ВОВСЕ (решение владельца): оно не
        // говорит о содержимом, смотреть в нём нечего, и строкой без имени
        // рода оно только мусорило бы список. С переходом чистки на гашение
        // адресом (m17) такая запись появляется после КАЖДОЙ чистки, а не
        // изредка, — и без этой строки её увидел бы каждый. ПОГАШЕННОЕ — тоже:
        // байт у него нет, показать нечего, а строкой оно врало бы, что вешка
        // есть (все остальные потребители журнала isVoided уже пропускают).
        if (!entry.statesContent() || journal.isVoided(i)) continue;
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
