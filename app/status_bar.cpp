#include "status_bar.h"

#include "doc_model.h"
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
    const ZSettings& a = settings();
    QFont font(a.look().statusFamily(), a.look().statusFontPoints());
    left_->setFont(font);
    right_->setFont(font);

    const QString colour = a.look().statusTextColor().name(QColor::HexRgb);
    left_->setStyleSheet(QStringLiteral("color: %1;").arg(colour));
    right_->setStyleSheet(QStringLiteral("color: %1;").arg(colour));

    auto* layout = qobject_cast<QHBoxLayout*>(this->layout());
    if (layout != nullptr) {
        layout->setContentsMargins(a.look().statusPadding(), a.look().statusPaddingTop(), a.look().statusPadding(),
                                   a.look().statusPaddingTop());
        layout->setSpacing(a.look().statusPadding());
    }
    setAutoFillBackground(true);
    QPalette pal = palette();
    pal.setColor(QPalette::Window, a.look().statusBackground());
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
                      info.colorSpace == image_.colorSpace && info.bits == image_.bits &&
                      info.taken == image_.taken;
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
    // Звёздочка занимает место и в обычном случае: без этого строка дёргалась
    // бы туда-сюда при каждой смене признака.
    const QString mark = note_.suspect ? QStringLiteral(" *") : QString();
    const int room = qMax(
        0, left_->width() - metrics.horizontalAdvance(rest + separator + QStringLiteral(" *")));
    const QString shown = metrics.elidedText(name, Qt::ElideMiddle, room);
    const QString plain = shown.isEmpty() ? rest : shown + mark + separator + rest;

    if (!note_.suspect) {
        left_->setTextFormat(Qt::PlainText);
        left_->setText(plain);
    } else {
        // Красная только ЗВЁЗДОЧКА, а не вся строка: строка говорит про
        // заметку, звёздочка — про беду. Разметкой, потому что цвет нужен
        // куску текста, а не всей надписи; всё остальное экранируется.
        left_->setTextFormat(Qt::RichText);
        left_->setText(shown.toHtmlEscaped() +
                       QStringLiteral(" <span style=\"color:%1\">*</span>")
                           .arg(settings().look().statusSuspectColor().name(QColor::HexRgb)) +
                       QString(separator + rest).toHtmlEscaped());
    }
    left_->setToolTip(note_.suspect
                          ? QStringLiteral("%1\n\nСамопроверка при записи не сошлась: "
                                           "разобранное обратно отличается от документа. "
                                           "Заметка записана, копия буфера — в .rescue. "
                                           "Звёздочка погаснет, как только очередная запись "
                                           "сойдётся.")
                                .arg(note_.path)
                          : note_.path);
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
    // Формата в строке нет намеренно: расширение в имени файла говорит о нём
    // однозначно, и «01n6….jxl · JXL» — это одно и то же слово дважды.
    head << image_.name << humanBytes(image_.bytes);
    if (!image_.size.isEmpty())
        head << QStringLiteral("%1×%2").arg(image_.size.width()).arg(image_.size.height());

    // Цвет и глубина — вместе и отдельной секцией: это про то, как записаны
    // сами отсчёты. Известны они только у разжатой копии; пока картинку не
    // показывали, их просто нет, и придумывать их нельзя.
    QStringList samples;
    if (!image_.colorSpace.isEmpty()) samples << image_.colorSpace;
    if (image_.bits > 0) samples << QStringLiteral("%1 бит").arg(image_.bits);
    if (image_.frames > 1) samples << QStringLiteral("кадров %1").arg(humanCount(image_.frames));
    if (!samples.isEmpty()) head << samples.join(QLatin1Char(' '));

    // КОГДА СНЯТО — тем же словом и тем же видом, что у заметки: «создана
    // DD.MM.YYYY HH:MM». Правки у снимка мы не показываем: у заметки правка —
    // её собственная история, а у вложения это время файла, к содержимому
    // снимка отношения не имеющее.
    //
    // Нет метаданных — строки нет вовсе. Придумывать дату из времени файла
    // нельзя: это было бы время копирования, а сказано «создана».
    if (image_.taken.isValid())
        head << QStringLiteral("создана %1").arg(humanDate(image_.taken));

    const QString known = head.join(separator);
    // ПОДПИСЬ ЗДЕСЬ ТОЛЬКО ТОГДА, КОГДА ЕЁ НЕ ВИДНО ПОД СНИМКОМ. Обычно она
    // стоит под фотографией (imageCaption), и повторять её в панели — значит
    // говорить одно и то же дважды. Выключил человек подпись под снимком —
    // панель остаётся единственным местом, где она вообще есть, и молчать
    // тогда нельзя.
    // Безымянную («IMG_1234», «~спрятана») под снимком тоже не видно — а в
    // панели она к месту: это справка о файле, и человеку видно, что подпись
    // у снимка есть и какая.
    const bool underPhoto = settings().look().imageCaption() && !isNonameCaption(image_.caption);
    const QString caption = underPhoto ? QString() : image_.caption;
    left_->setToolTip(caption.isEmpty() ? known : known + separator + caption);
    if (caption.isEmpty()) {
        left_->setText(known);
        return;
    }

    // Режется ТОЛЬКО описание, и только с конца: оно бывает длиной в абзац, а
    // всё, что слева, — это про сам файл, и терять его нельзя.
    const QFontMetrics metrics(left_->font());
    const int room =
        qMax(0, left_->width() - metrics.horizontalAdvance(known + separator));
    const QString shown = metrics.elidedText(caption, Qt::ElideRight, room);
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
    // «Картинка стоит тысячи слов», а в счёте слов она стоит нуля — поэтому
    // про них говорим отдельно и только когда они есть.
    const QString photos = note_.images > 0
                               ? QStringLiteral("   ·   изображений %1").arg(humanCount(note_.images))
                               : QString();
    right_->setText(QStringLiteral("слов %1%2   ·   строка %3/%4   ·   кол %5")
                        .arg(words, photos, humanCount(line_), humanCount(note_.lines),
                             humanCount(column_)));
}

void StatusBar::resizeEvent(QResizeEvent* e) {
    QWidget::resizeEvent(e);
    showLeft();   // ширина изменилась — многоточие переезжает
}

void StatusBar::paintEvent(QPaintEvent* e) {
    QWidget::paintEvent(e);
    QPainter painter(this);
    painter.setPen(settings().look().statusSeparatorColor());
    // Ровно одна ЛОГИЧЕСКАЯ точка сверху, как черта под тулбаром.
    painter.drawLine(0, 0, width(), 0);
}

}  // namespace zametti
