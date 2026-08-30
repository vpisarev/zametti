#include "zapp.h"

#include "resources.h"

#include <QFileInfo>
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
    // Секция "stores" из state.json — менеджеру: канонизация и дедупликация
    // одни, его. Связка приедет позже (setStoreSecrets из main), когда main
    // решит, keyring это или среда.
    stores_.storesFromJson(state_.storesJson());
    applySettingsToCaches();
}

std::shared_ptr<ZStorage> ZApp::openStorage(const QString& root) {
    storage_ = std::make_shared<ZStorage>(root);
    storage_->reload();
    addInfoFolder(*storage_);
    return storage_;
}

// ДОКУМЕНТАЦИЯ — ПАПКА ДЕРЕВА, А НЕ ОКОШКО. Заводится здесь, потому что здесь
// рождается всякое хранилище — и настоящее, и пустое (корень пуст, окно без
// хранилища): папка Info обязана быть на месте в обоих случаях, а «оба случая»
// сходятся ровно в одной строке выше.
//
// Что вшито — решает CMake обходом docs/info/; здесь нет ни списка файлов, ни
// списка заголовков. Заголовок каждой строки хранилище спросит у самого
// документа.
void ZApp::addInfoFolder(ZStorage& storage) {
    ZStorage::VirtualFolder info;
    info.id = QStringLiteral("info");
    info.title = QStringLiteral("Info");
    info.icon = QStringLiteral("badge-info");
    info.readOnly = true;
    if (!infoReady_) {
        for (const QString& path : embeddedDocs()) {
            ZStorage::VirtualNote note;
            note.id = info.id + QLatin1Char(':') + QFileInfo(path).completeBaseName();
            note.path = path;
            infoNotes_.push_back(std::move(note));
        }
        // Заголовки соберёт само хранилище — и мы заберём их обратно, чтобы
        // больше не считать: разбор двух наших документов стоит 20–40 мс в
        // Debug, а хранилище открывается ещё и на каждом переключении.
        info.notes = infoNotes_;
        storage.addVirtualFolder(info);
        const ZStorage::VirtualFolder* ready = storage.virtualFolder(info.id);
        if (ready != nullptr) infoNotes_ = ready->notes;
        infoReady_ = true;
        return;
    }
    info.notes = infoNotes_;
    storage.addVirtualFolder(std::move(info));
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
    // Пределы логов — из свежих настроек; смена предела заново взводит
    // проверку переполнения (она случится при первой же записи).
    ZLogs::instance().configure({settings().logs().writeErrLog(),
                                 settings().logs().writeSyncLog(),
                                 qint64(settings().logs().logSizeMb()) * 1024 * 1024});
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
