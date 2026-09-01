#include "image_viewer.h"

#include "image_read.h"
#include "settings.h"

#include <QFileInfo>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QScreen>

namespace zametti {

ImageViewer::ImageViewer(QWidget* parent)
    : QWidget(parent, Qt::Window | Qt::FramelessWindowHint) {
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_DeleteOnClose, false);
    setWindowTitle(QStringLiteral("zametti"));
}

bool ImageViewer::show(std::vector<Shot> shots, int at) {
    if (shots.empty()) return false;
    shots_ = std::move(shots);
    at_ = qBound(0, at, int(shots_.size()) - 1);
    load();
    showFullScreen();
    raise();
    activateWindow();
    setFocus(Qt::OtherFocusReason);
    return true;
}

void ImageViewer::refreshAppearance() { update(); }

void ImageViewer::load() {
    shown_ = QImage();
    failed_.clear();
    const QString path = shots_[size_t(at_)].path;
    // Читаем СВОИМИ читателями (jxl, avif, heic и прочие) — теми же, что и
    // заметка: Qt половину наших форматов не знает.
    DecodeRequest request;
    // Больше экрана держать в памяти незачем: показать всё равно негде.
    if (const QScreen* screen = QGuiApplication::primaryScreen(); screen != nullptr)
        request.maxSize = screen->size() * screen->devicePixelRatio();
    shown_ = decodeImageFile(path, request);
    if (shown_.isNull())
        failed_ = QStringLiteral("cannot read %1").arg(QFileInfo(path).fileName());
    update();
}

void ImageViewer::step(int delta) {
    if (shots_.size() < 2) return;
    const int count = int(shots_.size());
    at_ = (at_ + delta % count + count) % count;
    load();
}

void ImageViewer::keyPressEvent(QKeyEvent* event) {
    switch (event->key()) {
        case Qt::Key_Escape:
        case Qt::Key_F11:
        case Qt::Key_Q:
            close();
            emit closed();
            return;
        case Qt::Key_Left:
        case Qt::Key_Up:
        case Qt::Key_PageUp:
        case Qt::Key_Backspace:
            step(-1);
            return;
        case Qt::Key_Right:
        case Qt::Key_Down:
        case Qt::Key_PageDown:
        case Qt::Key_Space:
            step(1);
            return;
        case Qt::Key_Home:
            at_ = 0;
            load();
            return;
        case Qt::Key_End:
            at_ = int(shots_.size()) - 1;
            load();
            return;
        default:
            break;
    }
    QWidget::keyPressEvent(event);
}

void ImageViewer::mousePressEvent(QMouseEvent* event) {
    // Щелчок по правой половине — следующий снимок, по левой — предыдущий:
    // так листают везде, и мышью это быстрее, чем тянуться к стрелкам.
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }
    step(event->position().x() >= width() / 2.0 ? 1 : -1);
}

void ImageViewer::paintEvent(QPaintEvent*) {
    const ZSettings::ImageViewer& look = settings().imageViewer();
    QPainter painter(this);
    painter.fillRect(rect(), look.background());

    QFont captionFont(settings().ui().appFamily());
    captionFont.setPointSizeF(look.captionPoints());
    const QFontMetricsF metrics(captionFont);
    const QString caption = shots_.empty() ? QString() : shots_[size_t(at_)].caption;
    // Место под подписью отводится ВСЕГДА, даже когда её нет: иначе картинка
    // прыгала бы при листании между снимками с подписью и без.
    const qreal captionBand = metrics.height() + look.margin();
    const int margin = look.margin();
    const QRectF field(margin, margin, width() - 2.0 * margin,
                       height() - captionBand - 2.0 * margin);

    if (!shown_.isNull() && field.width() > 1 && field.height() > 1) {
        // Крупная картинка ужимается по экрану; мелкая увеличивается, но не
        // больше потолка (imageViewer.maxZoomPercent): растянутый значок — это
        // каша, а не показ.
        const qreal fit = qMin(field.width() / shown_.width(), field.height() / shown_.height());
        const qreal ceiling = look.maxZoomPercent() / 100.0;
        const qreal scale = qMin(fit, ceiling);
        const QSizeF size(shown_.width() * scale, shown_.height() * scale);
        const QRectF where(field.center().x() - size.width() / 2,
                           field.center().y() - size.height() / 2, size.width(), size.height());
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        painter.drawImage(where, shown_);
    } else if (!failed_.isEmpty()) {
        painter.setFont(captionFont);
        painter.setPen(look.captionColor());
        painter.drawText(field, Qt::AlignCenter, failed_);
    }

    // Подпись и счётчик снизу: слева подпись, справа «3/7» — сколько ещё
    // снимков в заметке, видно сразу.
    painter.setFont(captionFont);
    painter.setPen(look.captionColor());
    const QRectF bottom(margin, height() - captionBand, width() - 2.0 * margin, captionBand);
    if (!caption.isEmpty())
        painter.drawText(bottom, Qt::AlignLeft | Qt::AlignVCenter,
                         metrics.elidedText(caption, Qt::ElideRight, int(bottom.width() * 0.8)));
    if (shots_.size() > 1)
        painter.drawText(bottom, Qt::AlignRight | Qt::AlignVCenter,
                         QStringLiteral("%1/%2").arg(at_ + 1).arg(shots_.size()));
}

}  // namespace zametti
