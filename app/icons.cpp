#include "icons.h"

#include "resources.h"

#include <QHash>
#include <QPainter>
#include <QSvgRenderer>

namespace zametti {
namespace {

// Ключ кэша. Плотность экрана попадает в него округлённой до сотых: она
// приходит дробной (1.25, 1.5, 2.0), и сравнивать её как есть — приглашение
// держать в памяти по растру на каждое случайное значение.
struct Key {
    QString name;
    int points;
    QRgb color;
    int dprHundredths;

    bool operator==(const Key& other) const = default;
};

size_t qHash(const Key& k, size_t seed = 0) {
    return qHashMulti(seed, k.name, k.points, k.color, k.dprHundredths);
}

QHash<Key, QPixmap> g_cache;

}  // namespace

QPixmap toolbarIcon(const QString& name, int points, const QColor& color, qreal dpr) {
    if (points <= 0) return QPixmap();
    if (dpr <= 0) dpr = 1.0;

    const Key key{name, points, color.rgba(), int(qRound(dpr * 100))};
    const auto found = g_cache.constFind(key);
    if (found != g_cache.constEnd()) return found.value();

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
    g_cache.insert(key, pixmap);
    return pixmap;
}

void clearIconCache() { g_cache.clear(); }

int iconCacheSize() { return int(g_cache.size()); }

}  // namespace zametti
