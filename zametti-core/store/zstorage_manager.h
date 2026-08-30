// ZStorageManager — список хранилищ устройства и ФАКТЫ его строк (ядро).
//
// Это модель для окна хранилищ (StoreManagerModel — вид, StoreJobRunner —
// работы) и хозяин раздела "stores": [...] в state.json: он один канонизирует
// пути, дедуплицирует строки и отвечает, что лежит по названному пути и что
// про него знает связка. Сам файл state.json по-прежнему пишет ТОЛЬКО ZApp
// (правило проекта): менеджер лишь отдаёт и принимает свою секцию
// (storesToJson/storesFromJson).
//
// ФАКТЫ — ЭТО НЕ ЧЕРНОВИКИ. Черновики (что человек набрал в форме) живут в
// StoreManagerModel; здесь — только то, что можно узнать у диска и связки:
// какого рода каталог (inspect), сводка хранилища (localSummary), есть ли в
// связке ключ и пароли (SecretStore::has — атрибуты, БЕЗ чтения секрета:
// на маке чтение вправе поднять системный вопрос, а спрашивается это на
// каждое переключение строки). Ответы кэшируются по каноническому пути;
// refresh() сбрасывает кэш строки после работы, которая могла факты сменить.

#ifndef ZAMETTI_ZSTORAGE_MANAGER_H
#define ZAMETTI_ZSTORAGE_MANAGER_H

#include "zstorage.h"

#include <QHash>
#include <QJsonArray>
#include <QList>
#include <QString>

#include <memory>

namespace zametti {

class SecretStore;

class ZStorageManager {
public:
    // Связка нужна фактам (Known); без неё они честно Unknown. Отдаётся
    // указателем, а не читается глобально: наборы подсовывают подделку.
    explicit ZStorageManager(std::shared_ptr<SecretStore> secrets = nullptr);
    // Связка приезжает и позже (ZApp живёт раньше, чем main решит, keyring
    // это или среда); прежние ответы Known пересчитываются.
    void setSecrets(std::shared_ptr<SecretStore> secrets);

    // Ключ строки — канонический вид пути: один и тот же каталог, названный
    // с хвостовым слэшем или через «..», не должен плодить две строки.
    static QString canonicalRoot(const QString& root);

    // --- список ------------------------------------------------------------
    const QList<ZStorage::Config>& stores() const { return stores_; }
    int size() const { return int(stores_.size()); }
    bool isEmpty() const { return stores_.isEmpty(); }
    // Без дублей по канонизированному root; знакомая строка обновляется НА
    // МЕСТЕ (порядок стабилен — список в диалоге не прыгает), новая — в конец.
    // Имя, не приехавшее с записью, наследуется от прежней: прежнее дороже
    // пустого.
    void remember(const ZStorage::Config& entry);
    void forget(const QString& root);
    // Строка по корню; неизвестный корень — пустая запись.
    ZStorage::Config storeFor(const QString& root) const;

    // --- секция "stores" в state.json ---------------------------------------
    QJsonArray storesToJson() const;
    void storesFromJson(const QJsonArray& stores);

    // --- факты строки --------------------------------------------------------
    // Тройка ответов связки: спрошено — да/нет; связки нет — неизвестно.
    enum class Known { Unknown, Yes, No };
    struct Facts {
        ZStorage::DirKind kind = ZStorage::DirKind::Missing;
        ZStorage::Summary stats;              // пусты, когда kind != Store
        Known key = Known::Unknown;           // мастер-ключ в связке?
        Known serverPassword = Known::Unknown;
        Known encryptionPassword = Known::Unknown;
    };
    // Факты ЛЮБОГО названного пути, не только строки списка: человек мог
    // набрать другую папку в форме, и показывать он вправе то, что набрано.
    // Ответ кэшируется по каноническому пути до refresh()/remember()/forget().
    Facts facts(const QString& folder);
    // Работа могла сменить факты (завелось хранилище, лёг секрет): сбросить
    // кэш строки обязан кто-то один — вот он; пересчитает следующий facts().
    void refresh(const QString& root);

protected:
    // storeId каталога — чтением zametti.json, с кэшем: identity стоит чтения
    // файла, а facts спрашиваются на каждое переключение строки.
    QString storeIdOf(const QString& key);

    std::shared_ptr<SecretStore> secrets_;
    QList<ZStorage::Config> stores_;
    QHash<QString, Facts> factsCache_;
    QHash<QString, QString> storeIds_;
};

}  // namespace zametti

#endif  // ZAMETTI_ZSTORAGE_MANAGER_H
