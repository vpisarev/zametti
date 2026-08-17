#include "zapp.h"

#include "resources.h"

#include <QImage>
#include <QPainter>
#include <QSvgRenderer>

namespace zametti {

namespace {
ZApp* g_app = nullptr;   // единственный указатель на объект приложения
}

size_t qHash(const ZApp::IconKey& k, size_t seed) {
    return qHashMulti(seed, k.name, k.points, k.color, k.dprHundredths);
}

ZApp::ZApp() : state_(ZAppState::load()) {
    if (g_app == nullptr) g_app = this;
    applySettingsToCaches();
}

std::shared_ptr<ZStorage> ZApp::openStorage(const QString& root) {
    storage_ = std::make_shared<ZStorage>(root);
    storage_->reload();
    return storage_;
}

void ZApp::applySettingsToCaches() {
    const ZSettings::Cache& cache = settings().cache();
    images_.setLimits(cache.imageCacheSizeMb(), cache.maxLoadedImageSize());
}

ZApp::~ZApp() {
    if (g_app == this) g_app = nullptr;
}

ZApp& ZApp::instance() {
    if (g_app == nullptr) {
        // Наборы и утилиты объект приложения не заводят: заводим сами, один на
        // процесс, и он живёт до конца процесса.
        static ZApp fallback;
        g_app = &fallback;
    }
    return *g_app;
}

bool ZApp::reloadSettings(QString* error, QStringList* unknown) {
    const bool ok = loadSettings(error, unknown);
    applySettingsToCaches();
    return ok;
}

QPixmap ZApp::toolbarIcon(const QString& name, int points, const QColor& color, qreal dpr) {
    if (points <= 0) return QPixmap();
    if (dpr <= 0) dpr = 1.0;

    const IconKey key{name, points, color.rgba(), int(qRound(dpr * 100))};
    const auto found = icons_.constFind(key);
    if (found != icons_.constEnd()) return found.value();

    const qreal exactDpr = key.dprHundredths / 100.0;
    const int px = int(qRound(points * exactDpr));

    QImage image(px, px, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    {
        QSvgRenderer renderer(iconPath(name.toLatin1().constData()));
        if (!renderer.isValid()) return QPixmap();
        QPainter painter(&image);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        renderer.render(&painter, QRectF(0, 0, px, px));
        // Перекраска по альфе: SourceIn оставляет прозрачность нарисованного и
        // подменяет цвет. Штрих у Lucide объявлен currentColor, и без этого шага
        // Qt рисует его чёрным — на тёмной теме иконка исчезла бы.
        painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
        painter.fillRect(image.rect(), color);
    }

    QPixmap pixmap = QPixmap::fromImage(image);
    // Плотность ставится на готовый растр: без неё Qt считает его логическим
    // размером px и рисует иконку вдвое крупнее задуманного.
    pixmap.setDevicePixelRatio(exactDpr);
    icons_.insert(key, pixmap);
    return pixmap;
}

void ZApp::clearIconCache() { icons_.clear(); }

}  // namespace zametti
