#include "status_bar.h"

#include "settings.h"

#include <QFileInfo>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QResizeEvent>

namespace zametti {
namespace {

// Неразрывный пробел между числом и единицей и внутри разрядов: перенос строки
// посреди «12,4 КБ» читается как два разных числа.
const QChar kNbsp(0x00A0);

}  // namespace

QString humanBytes(qint64 bytes) {
    if (bytes < 1024) return QStringLiteral("%1%2Б").arg(bytes).arg(kNbsp);
    const double kb = double(bytes) / 1024.0;
    if (kb < 1024.0)
        return QStringLiteral("%1%2КБ").arg(kb, 0, 'f', kb < 10.0 ? 1 : 0).arg(kNbsp);
    const double mb = kb / 1024.0;
    return QStringLiteral("%1%2МБ").arg(mb, 0, 'f', mb < 10.0 ? 1 : 0).arg(kNbsp);
}

QString humanCount(int value) {
    QString digits = QString::number(value);
    for (int at = digits.size() - 3; at > 0; at -= 3) digits.insert(at, kNbsp);
    return digits;
}

QString humanDate(const QDateTime& when) {
    if (!when.isValid()) return {};
    return when.toLocalTime().toString(QStringLiteral("dd.MM.yyyy HH:mm"));
}

StatusBar::StatusBar(QWidget* parent) : QWidget(parent) {
    left_ = new QLabel(this);
    right_ = new QLabel(this);
    // Выделять мышью можно: путь к заметке иногда нужно скопировать.
    left_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    right_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    // Левое поле сжимается до нуля, правое — никогда: числа не режутся.
    left_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    right_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);

    auto* layout = new QHBoxLayout(this);
    layout->addWidget(left_, 1);
    layout->addWidget(right_, 0);
    refreshAppearance();
}

void StatusBar::refreshAppearance() {
    const Appearance& a = appearance();
    QFont font(a.statusFamily, a.statusFontPoints);
    left_->setFont(font);
    right_->setFont(font);

    const QString colour = a.statusTextColor.name(QColor::HexRgb);
    left_->setStyleSheet(QStringLiteral("color: %1;").arg(colour));
    right_->setStyleSheet(QStringLiteral("color: %1;").arg(colour));

    auto* layout = qobject_cast<QHBoxLayout*>(this->layout());
    if (layout != nullptr) {
        layout->setContentsMargins(a.statusPadding, a.statusPaddingTop, a.statusPadding,
                                   a.statusPaddingTop);
        layout->setSpacing(a.statusPadding);
    }
    setAutoFillBackground(true);
    QPalette pal = palette();
    pal.setColor(QPalette::Window, a.statusBackground);
    setPalette(pal);
    relayout();
}

void StatusBar::setNote(const NoteInfo& info) {
    note_ = info;
    relayout();
}

void StatusBar::setCaret(int line, int column) {
    if (line == line_ && column == column_) return;
    line_ = line;
    column_ = column;
    relayout();
}

void StatusBar::setMessage(const QString& text) {
    if (text == message_) return;
    message_ = text;
    showLeft();
}

void StatusBar::showLeft() {
    if (!message_.isEmpty()) {
        left_->setText(message_);
        return;
    }
    if (!note_.valid) {
        left_->setText(QString());
        return;
    }
    // Обрезается ТОЛЬКО путь, и только он. Резать строку целиком нельзя: в
    // первом же снимке многоточие съело и размер, и обе даты, оставив один
    // хвост пути — то есть выкинуло как раз то, чего в пути и не видно.
    QStringList tail;
    tail << humanBytes(note_.bytes);
    if (note_.created.isValid())
        tail << QStringLiteral("создана %1").arg(humanDate(note_.created));
    if (note_.modified.isValid())
        tail << QStringLiteral("правлена %1").arg(humanDate(note_.modified));

    const QString separator = QStringLiteral("   ·   ");
    const QString rest = tail.join(separator);
    const QFontMetrics metrics(left_->font());
    // Только ИМЯ файла, без пути (решение владельца): начало пути у всех
    // заметок хранилища одинаковое, а места ест много. Полный путь остаётся в
    // подсказке — скопировать его иногда нужно.
    const QString name = QFileInfo(note_.path).fileName();
    const int room = qMax(0, left_->width() - metrics.horizontalAdvance(rest + separator));
    const QString shown = metrics.elidedText(name, Qt::ElideMiddle, room);
    left_->setText(shown.isEmpty() ? rest : shown + separator + rest);
    left_->setToolTip(note_.path);
}

void StatusBar::relayout() {
    showLeft();
    if (!note_.valid) {
        right_->setText(QString());
        return;
    }
    // «?» вместо числа слов — это не заглушка на будущее, а признак: счёт
    // отстал от документа и будет пересчитан ближайшим сохранением.
    const QString words = note_.wordsKnown ? humanCount(note_.words) : QStringLiteral("?");
    right_->setText(QStringLiteral("слов %1   ·   строка %2 из %3   ·   знак %4")
                        .arg(words, humanCount(line_), humanCount(note_.lines),
                             humanCount(column_)));
}

void StatusBar::resizeEvent(QResizeEvent* e) {
    QWidget::resizeEvent(e);
    showLeft();   // ширина изменилась — многоточие переезжает
}

void StatusBar::paintEvent(QPaintEvent* e) {
    QWidget::paintEvent(e);
    QPainter painter(this);
    painter.setPen(appearance().statusSeparatorColor);
    // Ровно одна ЛОГИЧЕСКАЯ точка сверху, как черта под тулбаром.
    painter.drawLine(0, 0, width(), 0);
}

}  // namespace zametti
