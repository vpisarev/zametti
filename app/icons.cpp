#include "icons.h"

#include "zapp.h"

namespace zametti {

// Кэш иконок принадлежит объекту приложения (ZApp); здесь — только удобный
// вход по имени, тот же, каким иконки просили всегда.
QPixmap toolbarIcon(const QString& name, int points, const QColor& color, qreal dpr) {
    return ZApp::instance().toolbarIcon(name, points, color, dpr);
}

void clearIconCache() { ZApp::instance().clearIconCache(); }

int iconCacheSize() { return ZApp::instance().iconCacheSize(); }

}  // namespace zametti
