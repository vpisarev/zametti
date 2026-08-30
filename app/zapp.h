// ZApp — объект приложения. Один на процесс (приложение — синглетон, решение
// владельца), создаётся в main() и доступен через ZApp::instance().
//
// Ему принадлежит то, что раньше лежало глобальными переменными и полями
// чужих виджетов (аудит refactor2, §1.1):
//
//   * настройки — читаются settings() (только чтение), перечитываются здесь;
//   * состояние сеанса (state.json) — ЕДИНЫЕ ВОРОТА К ЗАПИСИ: пишет только
//     ZApp, окно лишь наполняет поля перед выходом;
//   * место каретки по заметкам: хранит его ZNote, а когда заметка уходит из
//     LRU-кэша открытых (или программа выходит), ZApp кладёт его в state.json
//     — без дублей, по id заметки — и отдаёт при следующем открытии;
//   * кэш иконок тулбара (растеризованные SVG по имени, кеглю, цвету и
//     плотности);
//   * впереди — кэш картинок (из NoteView), движок формул.
//
// С каталогом хранилища ZApp не разговаривает: это дело ZStorage.

#ifndef ZAMETTI_ZAPP_H
#define ZAMETTI_ZAPP_H

#include "zlogs.h"
#include "app_state.h"
#include "settings.h"
#include "zimage_cache.h"
#include "zstorage.h"
#include "zstorage_manager.h"

#include <memory>

#include <QColor>
#include <QHash>
#include <QPixmap>
#include <QString>
#include <QStringList>


namespace zametti {

class ZApp {
public:
    ZApp();
    ~ZApp();
    ZApp(const ZApp&) = delete;
    ZApp& operator=(const ZApp&) = delete;

    // Объект, созданный main(); если его нет (наборы, утилиты) — заводится
    // свой, один на процесс.
    static ZApp& instance();

    // --- настройки ---------------------------------------------------------
    // Перечитать конфиг (первый раз — при старте, потом — по сторожу файла).
    // Ложь — файл битый, настройки прежние; объяснение в error.
    bool reloadSettings(QString* error, QStringList* unknown = nullptr);

    // --- состояние сеанса (state.json) --------------------------------------
    // ZAppState прочитано один раз при создании; окно правит поля и зовёт
    // saveState() на выходе — единственная запись state.json в программе.
    // Каретки по заметкам (rememberCaret/caretOf) — там же, по id заметки.
    ZAppState& state() { return state_; }
    const ZAppState& state() const { return state_; }
    // Секция "stores" принадлежит менеджеру: перед записью она забирается у
    // него, файл же пишет по-прежнему только ZApp (единые ворота).
    void saveState() {
        state_.setStoresJson(stores_.storesToJson());
        state_.save();
    }

    // --- список хранилищ устройства ------------------------------------------
    // ZStorageManager — модель окна хранилищ и хозяин секции "stores" в
    // state.json; наполняется из state при создании ZApp. Связку ему отдаёт
    // main() (setStoreSecrets), когда решит, keyring это или среда: без неё
    // факты строк честно отвечают Unknown.
    ZStorageManager& stores() { return stores_; }
    void setStoreSecrets(std::shared_ptr<SecretStore> secrets) {
        stores_ = ZStorageManager(std::move(secrets));
        stores_.storesFromJson(state_.storesJson());
    }

    // --- хранилище -----------------------------------------------------------
    // Открытое хранилище (или каталог/файл вне хранилища — тогда storage()
    // отвечает isStore() ложью). Одно на программу; кто-то другой хранилища не
    // открывает — с его каталогом говорит только ZStorage.
    std::shared_ptr<ZStorage> openStorage(const QString& root);
    std::shared_ptr<ZStorage> storage() const { return storage_; }
    // Виртуальная папка Info — вшитая документация. Заводится при открытии
    // всякого хранилища, включая пустое: справка есть и когда хранилища нет.
    // Открыто наружу ради наборов, которые заводят хранилище сами.
    //
    // СОБИРАЕТСЯ ОДИН РАЗ НА ПРОЦЕСС. Заголовок строки — первая строка самого
    // документа, и узнаётся он единственным честным способом: документ
    // разбирается и собирается (ZNote::load), второго пути «из markdown в
    // заголовок» в программе нет. Стоит это заметно — 20–40 мс на два наших
    // документа в Debug (замерено набором InfoFolder), и растёт с их размером
    // и числом, — а хранилище
    // открывают не только на старте, но и всякий раз при переключении.
    // Документы при этом вшиты в бинарник и за время работы не меняются:
    // значит, считать их заголовки больше одного раза незачем.
    void addInfoFolder(ZStorage& storage);

    // --- логи ----------------------------------------------------------------
    // err.log и sync.log рядом с config.json; экземпляр один на программу
    // (ZLogs::instance() — им же пользуются getLogStream и CLI); конфиг
    // перечитан — пределы перечитаны (reloadSettings зовёт configure).
    ZLogs& logs() { return ZLogs::instance(); }

    // --- кэш картинок --------------------------------------------------------
    // Один на программу; бюджет и предел стороны — из settings().cache(),
    // ставятся здесь при старте и при перечитывании конфига.
    ZImageCache& images() { return images_; }
    // Применить нынешние настройки к кэшам (бюджеты). Зовётся при старте и при
    // перечитывании конфига; наборам — после подмены настроек люком.
    void applySettingsToCaches();

    // --- кэш иконок ----------------------------------------------------------
    // Растр иконки name (SVG из ресурсов) кеглем points, цветом color, при
    // плотности dpr; считается один раз на четвёрку и живёт до сброса.
    QPixmap toolbarIcon(const QString& name, int points, const QColor& color, qreal dpr);
    void clearIconCache();
    int iconCacheSize() const { return int(icons_.size()); }

protected:
    struct IconKey {
        QString name;
        int points = 0;
        QRgb color = 0;
        int dprHundredths = 100;
        bool operator==(const IconKey& other) const = default;
    };
    friend size_t qHash(const IconKey& k, size_t seed);

    ZAppState state_;
    ZStorageManager stores_;
    std::shared_ptr<ZStorage> storage_;
    // Готовая папка документации: собрана при первом обращении, дальше
    // раздаётся копией всякому новому хранилищу.
    std::vector<ZStorage::VirtualNote> infoNotes_;
    bool infoReady_ = false;
    ZImageCache images_;
    QHash<IconKey, QPixmap> icons_;
};

}  // namespace zametti

#endif  // ZAMETTI_ZAPP_H
