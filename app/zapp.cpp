#include "zapp.h"

#include "resources.h"

#include <QFileInfo>
#include <QImage>
#include <QMessageBox>
#include <QPushButton>
#include <QPainter>
#include <QSvgRenderer>

namespace zametti {

namespace {
ZApp* g_app = nullptr;   // единственный указатель на объект приложения
}

size_t qHash(const ZApp::IconKey& k, size_t seed) {
    return qHashMulti(seed, k.name, k.points, k.color, k.dprHundredths);
}

ZApp::ZApp() {
    if (g_app == nullptr) g_app = this;
    // Секция "stores" из state.json уходит менеджеру прямо при чтении и нигде
    // не хранится массивом. Связка приедет позже (setStoreSecrets из main),
    // когда main решит, keyring это или среда.
    state_ = ZAppState::load(storeManager_.get());
    applySettingsToCaches();
}

std::shared_ptr<ZStorage> ZApp::openStorage(const QString& root) {
    storage_ = std::make_shared<ZStorage>(root);
    storage_->reload();
    addInfoFolder(*storage_);
    // Открытое хранилище — первоклассное понятие менеджера (решение владельца,
    // 30.08.2026): ставится здесь, в единственном месте рождения хранилища —
    // и настоящего, и пустого (пустой корень = ничего не открыто).
    storeManager_->setOpenRoot(root);
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

QMessageBox* ZApp::messageBox(QWidget* parent, const QString& text, Notice kind) {
    auto* box = new QMessageBox(parent);
    // Рисует Qt: родное окно системы само решает про значок (на маке — значок
    // приложения, у голого бинаря — значок исполняемого файла) и кнопки, и
    // одинаковыми окна на трёх системах не бывают.
    box->setOption(QMessageBox::Option::DontUseNativeDialog, true);
    switch (kind) {
        case Notice::Question: box->setIcon(QMessageBox::Question); break;
        case Notice::Warning: box->setIcon(QMessageBox::Warning); break;
        case Notice::Information: box->setIcon(QMessageBox::Information); break;
    }
    box->setWindowTitle(QStringLiteral("zametti"));
    box->setText(text);
    box->setFont(uiStyle().appFont());
    return box;
}

void ZApp::warn(QWidget* parent, const QString& text) {
    QMessageBox* box = messageBox(parent, text, Notice::Warning);
    box->setStandardButtons(QMessageBox::Ok);
    box->exec();
    delete box;
}

void ZApp::inform(QWidget* parent, const QString& text) {
    QMessageBox* box = messageBox(parent, text, Notice::Information);
    box->setStandardButtons(QMessageBox::Ok);
    box->exec();
    delete box;
}

void ZApp::ask(QWidget* parent, const QString& question, std::function<void(bool)> answered,
               const QString& yes, const QString& no) {
    QMessageBox* box = messageBox(parent, question, Notice::Question);
    box->setAttribute(Qt::WA_DeleteOnClose);
    QPushButton* agree = box->addButton(yes, QMessageBox::YesRole);
    QPushButton* refuse = box->addButton(no, QMessageBox::NoRole);
    box->setDefaultButton(refuse);
    box->setEscapeButton(refuse);
    QObject::connect(box, &QMessageBox::finished, box,
                     [box, agree, answered = std::move(answered)](int) {
                         answered(box->clickedButton() == agree);
                     });
    box->open();
}

}  // namespace zametti
