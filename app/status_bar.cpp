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

void StatusBar::setImage(const ImageInfo& info) {
    // Движение каретки внутри одной картинки перекладывать строку не должно —
    // отсюда сравнение. Сравниваются ВСЕ поля, которые видны в строке: первый
    // заход сличал четыре из семи, и пропажа вложения (менялся только признак
    // «файл на месте») на панели не показывалась вовсе. Набор это и поймал.
    const bool same = info.valid == image_.valid && info.exists == image_.exists &&
                      info.name == image_.name && info.caption == image_.caption &&
                      info.format == image_.format && info.size == image_.size &&
                      info.bytes == image_.bytes && info.frames == image_.frames &&
                      info.colorSpace == image_.colorSpace && info.bits == image_.bits;
    if (same) return;
    image_ = info;
    showLeft();
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
    if (image_.valid) {
        showImage();
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

void StatusBar::showImage() {
    const QString separator = QStringLiteral("   ·   ");
    if (!image_.exists) {
        // Вложения нет: об этом и говорим. Заметка на него ссылается, место под
        // рамку держится, и молчать тут нельзя — иначе непонятно, почему вместо
        // снимка рамка.
        left_->setText(image_.name + separator + QStringLiteral("вложения нет"));
        left_->setToolTip(image_.name);
        return;
    }

    // Порядок владельца: имя · вес · «разрешение формат цвет глубина» · описание.
    // Внутри третьей группы разделитель — пробел: это всё про одно, про сами
    // пиксели, и точками оно бы рассыпалось.
    QStringList head;
    head << image_.name << humanBytes(image_.bytes);

    QStringList pixels;
    if (!image_.size.isEmpty())
        pixels << QStringLiteral("%1×%2").arg(image_.size.width()).arg(image_.size.height());
    if (!image_.format.isEmpty()) pixels << image_.format.toUpper();
    // Цвет и глубина известны только у разжатой копии. Пока картинку не
    // показывали, их просто нет — и придумывать их нельзя.
    if (!image_.colorSpace.isEmpty()) pixels << image_.colorSpace;
    if (image_.bits > 0) pixels << QStringLiteral("%1 бит").arg(image_.bits);
    if (image_.frames > 1) pixels << QStringLiteral("кадров %1").arg(humanCount(image_.frames));
    if (!pixels.isEmpty()) head << pixels.join(QLatin1Char(' '));

    const QString known = head.join(separator);
    left_->setToolTip(image_.caption.isEmpty() ? known : known + separator + image_.caption);
    if (image_.caption.isEmpty()) {
        left_->setText(known);
        return;
    }

    // Режется ТОЛЬКО описание, и только с конца: оно бывает длиной в абзац, а
    // всё, что слева, — это про сам файл, и терять его нельзя.
    const QFontMetrics metrics(left_->font());
    const int room =
        qMax(0, left_->width() - metrics.horizontalAdvance(known + separator));
    const QString shown = metrics.elidedText(image_.caption, Qt::ElideRight, room);
    left_->setText(shown.isEmpty() ? known : known + separator + shown);
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
    right_->setText(QStringLiteral("слов %1   ·   строка %2/%3   ·   кол %4")
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
