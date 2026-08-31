// KeyringSecrets на Linux: Secret Service API по QDBus, все секреты — ОДНОЙ
// записью-свёртком (заказ владельца 30.08.2026; правила и формат —
// bundled_secrets.h, secret_bundle.h; мак переехал первым, этот файл — вторым,
// 31.08.2026).
//
// Запись: item с атрибутами {application: zametti, kind: bundle}, метка
// zametti-secrets (витрина для seahorse). В данных — свёрток, в третьем
// атрибуте index — его индекс без секретов, base64(CBOR): атрибуты в a{ss}
// носят только строки. «Есть ли» (has) читается одними атрибутами, без
// GetSecret и, что важнее, БЕЗ Unlock — запертая связка не повод будить
// человека вопросом.
//
// ПОЛИТИКА ВОПРОСОВ — требование владельца: максимум один системный вопрос,
// и тот лишь когда связку не отпирает вход в сессию. Что это значит здесь:
//
//   * запись НИКОГДА не пересоздаётся: писатель идёт через Item.SetSecret на
//     найденном пути (+ Properties.Set для индекса), CreateItem — только для
//     самой первой записи в жизни устройства. CreateItem(replace) пересоздал
//     бы элемент, а с ним и ACL записи в демонах, которые его ведут;
//   * свёрток читается раз за прогон (кэш BundledSecrets) — Unlock, если он
//     и нужен, случится единожды, внутри явного жеста (глаз, Check, синк);
//   * available() отвечает «нет» и на ЗАПЕРТУЮ коллекцию: индекс запертой
//     связки может быть нечитаем (открытая часть файла gnome-keyring несёт
//     хеши атрибутов, не значения — меряется пробником), и «нет записи» из
//     него было бы враньём с красной тревогой в окне. Unknown честнее.
//
// МИГРАЦИЯ: прежде этот файл держал запись-на-секрет ({application, storeId,
// what}, метки zametti-{key,webdav,password}-<id>). Первое чтение свёртка,
// не найдя его, собирает старые записи в свёрток и, только записав его,
// удаляет их: оставленные, они превратили бы витрину связки и откат версии
// программы в источник протухших секретов. Мы уже внутри явного жеста чтения,
// так что Unlock миграции — тот же один.
//
// Наборы этот файл не гоняют (живая связка — вещь машины): правила свёртка
// покрывает bundled_secrets_test, приёмка ручная — `zametti-bench keyring`.

#include "bundled_secrets.h"
#include "keyring_secrets.h"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusInterface>
#include <QDBusMetaType>
#include <QDBusObjectPath>
#include <QDBusReply>
#include <QDBusVariant>
#include <QEventLoop>
#include <QTimer>

namespace zametti {
namespace {

constexpr char kService[] = "org.freedesktop.secrets";
constexpr char kServicePath[] = "/org/freedesktop/secrets";
constexpr char kServiceIface[] = "org.freedesktop.Secret.Service";
constexpr char kCollectionIface[] = "org.freedesktop.Secret.Collection";
constexpr char kItemIface[] = "org.freedesktop.Secret.Item";
constexpr char kPromptIface[] = "org.freedesktop.Secret.Prompt";
constexpr char kPropertiesIface[] = "org.freedesktop.DBus.Properties";

// Структура Secret из спецификации: (сессия, параметры, байты, content-type).
struct DBusSecret {
    QDBusObjectPath session;
    QByteArray parameters;
    QByteArray value;
    QString contentType;
};

QDBusArgument& operator<<(QDBusArgument& arg, const DBusSecret& secret) {
    arg.beginStructure();
    arg << secret.session << secret.parameters << secret.value << secret.contentType;
    arg.endStructure();
    return arg;
}

const QDBusArgument& operator>>(const QDBusArgument& arg, DBusSecret& secret) {
    arg.beginStructure();
    arg >> secret.session >> secret.parameters >> secret.value >> secret.contentType;
    arg.endStructure();
    return arg;
}

// Постоянные атрибуты записи-свёртка. Поиск идёт ТОЛЬКО по ним (subset-match
// спецификации): третий атрибут index меняется на каждой записи, и искать по
// нему значило бы не находить собственную запись.
QMap<QString, QString> bundleAttrs() {
    return {{QStringLiteral("application"), QStringLiteral("zametti")},
            {QStringLiteral("kind"), QStringLiteral("bundle")}};
}

void registerMetaTypes() {
    qDBusRegisterMetaType<DBusSecret>();
    qDBusRegisterMetaType<QMap<QString, QString>>();   // a{ss} — атрибуты
}

}  // namespace
}  // namespace zametti

// Q_DECLARE_METATYPE — только в глобальной области видимости.
Q_DECLARE_METATYPE(zametti::DBusSecret)

namespace zametti {
namespace {

// Двери BundledSecrets над Secret Service.
class DbusBundle : public BundledSecrets {
public:
    bool available() const override {
        if (!bus_.isConnected()) return false;
        const QDBusConnectionInterface* iface = bus_.interface();
        if (iface == nullptr) return false;
        // Служба либо уже на шине, либо активируется по обращению.
        const bool served =
            iface->isServiceRegistered(QLatin1String(kService)) ||
            [&] {
                const auto names = iface->activatableServiceNames();
                return names.isValid() && names.value().contains(QLatin1String(kService));
            }();
        if (!served) return false;
        // Запертая коллекция = «связки не видно» (Unknown), не «секретов нет»:
        // индекс запертой записи может быть нечитаем, и ложное «нет» зажгло бы
        // в окне красные требования на ровном месте (см. шапку).
        registerMetaTypes();
        const QDBusObjectPath coll = const_cast<DbusBundle*>(this)->resolveCollection(nullptr);
        if (coll.path().isEmpty()) return false;
        const QVariant locked = const_cast<DbusBundle*>(this)->collectionProperty(
            coll, QStringLiteral("Locked"));
        if (!locked.isValid()) return false;
        return !locked.toBool();
    }

protected:
    // --- три двери BundledSecrets ------------------------------------------

    bool readBundle(QByteArray* data, bool* found, QString* error) override {
        *found = false;
        if (!ensureSession(error)) return false;
        QString step;
        const QDBusObjectPath item = findBundle(true, &step);
        if (!item.path().isEmpty()) {
            const QByteArray bytes = getSecret(item, &step);
            if (!step.isEmpty()) {
                if (error != nullptr) *error = step;
                return false;
            }
            *data = bytes;
            *found = true;
            return true;
        }
        if (!step.isEmpty()) {
            if (error != nullptr) *error = step;
            return false;
        }
        // Свёртка нет. Единственный случай, когда это не «пустая связка», —
        // записи прежнего формата (по одной на секрет): собрать и переехать.
        return migrateLegacy(data, found, error);
    }

    bool writeBundle(const QByteArray& bundle, const QByteArray& index,
                     QString* error) override {
        if (!ensureSession(error)) return false;
        QMap<QString, QString> attrs = bundleAttrs();
        attrs.insert(QStringLiteral("index"),
                     QString::fromLatin1(index.toBase64()));

        DBusSecret secret;
        secret.session = session_;
        secret.value = bundle;
        secret.contentType = QStringLiteral("application/octet-stream");

        const QDBusObjectPath item = findBundle(true, error);
        if (!item.path().isEmpty()) {
            // Запись есть — переписать НА МЕСТЕ (см. шапку про ACL).
            QDBusInterface itemIface(kService, item.path(), kItemIface, bus_);
            QDBusReply<void> put = itemIface.call(QStringLiteral("SetSecret"),
                                                  QVariant::fromValue(secret));
            if (!put.isValid()) {
                if (error != nullptr)
                    *error = QStringLiteral("secret service: SetSecret failed: %1")
                                 .arg(put.error().message());
                return false;
            }
            QDBusInterface props(kService, item.path(), kPropertiesIface, bus_);
            QDBusReply<void> set = props.call(
                QStringLiteral("Set"), QLatin1String(kItemIface),
                QStringLiteral("Attributes"),
                QVariant::fromValue(QDBusVariant(QVariant::fromValue(attrs))));
            if (!set.isValid()) {
                if (error != nullptr)
                    *error = QStringLiteral(
                                 "secret service: setting the index failed: %1")
                                 .arg(set.error().message());
                return false;
            }
            return true;
        }
        if (error != nullptr && !error->isEmpty()) return false;

        // Самая первая запись в жизни устройства.
        QVariantMap properties;
        properties.insert(QStringLiteral("org.freedesktop.Secret.Item.Label"),
                          QStringLiteral("zametti-secrets"));
        properties.insert(QStringLiteral("org.freedesktop.Secret.Item.Attributes"),
                          QVariant::fromValue(attrs));
        QDBusInterface coll(kService, collection_.path(), kCollectionIface, bus_);
        QDBusMessage reply = coll.call(QStringLiteral("CreateItem"),
                                       QVariant::fromValue(properties),
                                       QVariant::fromValue(secret),
                                       false /* replace: пересоздавать нечего */);
        if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().size() < 2) {
            if (error != nullptr)
                *error = QStringLiteral("secret service: CreateItem failed: %1")
                             .arg(reply.errorMessage());
            return false;
        }
        return completePrompt(reply.arguments().at(1).value<QDBusObjectPath>(), error);
    }

    QByteArray readIndex() override {
        // ТОЛЬКО атрибуты — ни GetSecret, ни Unlock, ни сессии: SearchItems и
        // Properties.Get — методы, которым заперетость коллекции не мешает
        // (а если у демона мешает — атрибут просто не прочтётся, и available()
        // уже сказал «нет» на запертую коллекцию).
        if (!bus_.isConnected()) return {};
        registerMetaTypes();
        const QDBusObjectPath item = findBundle(false, nullptr);
        if (item.path().isEmpty()) return {};
        QDBusInterface props(kService, item.path(), kPropertiesIface, bus_);
        QDBusReply<QDBusVariant> got = props.call(QStringLiteral("Get"),
                                                  QLatin1String(kItemIface),
                                                  QStringLiteral("Attributes"));
        if (!got.isValid()) return {};
        const auto attrs =
            qdbus_cast<QMap<QString, QString>>(got.value().variant());
        return QByteArray::fromBase64(
            attrs.value(QStringLiteral("index")).toLatin1());
    }

private:
    // --- обвязка шины ------------------------------------------------------

    // Демон может показать человеку окно (разблокировка связки). Ответ
    // приходит сигналом Completed; ждём его, но не вечно — минута на ввод.
    //
    // «Отклонил ли человек» из сигнала не разбираем (динамические сигналы
    // D-Bus не дружат с лямбдами, а городить moc-объект ради флага не стоит):
    // отклонённая разблокировка честно провалит СЛЕДУЮЩУЮ операцию, и её
    // ошибка уедет вызывающему.
    bool completePrompt(const QDBusObjectPath& prompt, QString* error) {
        if (prompt.path().isEmpty() || prompt.path() == QLatin1String("/"))
            return true;
        QEventLoop loop;
        bus_.connect(kService, prompt.path(), kPromptIface,
                     QStringLiteral("Completed"), &loop, SLOT(quit()));
        QDBusInterface iface(kService, prompt.path(), kPromptIface, bus_);
        QDBusReply<void> shown = iface.call(QStringLiteral("Prompt"), QString());
        if (!shown.isValid()) {
            if (error != nullptr) *error = shown.error().message();
            return false;
        }
        QTimer::singleShot(60000, &loop, &QEventLoop::quit);
        loop.exec();
        return true;
    }

    QVariant collectionProperty(const QDBusObjectPath& coll, const QString& name) {
        QDBusInterface props(kService, coll.path(), kPropertiesIface, bus_);
        QDBusReply<QDBusVariant> got = props.call(
            QStringLiteral("Get"), QLatin1String(kCollectionIface), name);
        if (!got.isValid()) return {};
        return got.value().variant();
    }

    // Коллекция, в которой живёт запись: алиас default, а нет его — login по
    // известному пути, затем первая коллекция службы, кроме session (та
    // умирает с сеансом — свёртку в ней не место). Коллекций НЕ создаём и
    // SetAlias НЕ зовём: и то и другое — вопросы к человеку и его жилой зоне.
    QDBusObjectPath resolveCollection(QString* error) {
        QDBusInterface service(kService, kServicePath, kServiceIface, bus_);
        QDBusMessage aliasReply =
            service.call(QStringLiteral("ReadAlias"), QStringLiteral("default"));
        if (aliasReply.type() == QDBusMessage::ReplyMessage &&
            !aliasReply.arguments().isEmpty()) {
            const auto path = aliasReply.arguments().at(0).value<QDBusObjectPath>();
            if (!path.path().isEmpty() && path.path() != QLatin1String("/"))
                return path;
        }
        const QDBusObjectPath login(
            QStringLiteral("/org/freedesktop/secrets/collection/login"));
        if (collectionProperty(login, QStringLiteral("Label")).isValid())
            return login;
        QDBusInterface props(kService, kServicePath, kPropertiesIface, bus_);
        QDBusReply<QDBusVariant> all = props.call(QStringLiteral("Get"),
                                                  QLatin1String(kServiceIface),
                                                  QStringLiteral("Collections"));
        if (all.isValid()) {
            const auto paths =
                qdbus_cast<QList<QDBusObjectPath>>(all.value().variant());
            for (const QDBusObjectPath& p : paths)
                if (!p.path().endsWith(QLatin1String("/session"))) return p;
        }
        if (error != nullptr)
            *error = QStringLiteral("secret service: no usable collection");
        return {};
    }

    bool ensureSession(QString* error) {
        if (!session_.path().isEmpty()) return true;
        if (!bus_.isConnected()) {
            if (error != nullptr) *error = QStringLiteral("no D-Bus session bus");
            return false;
        }
        registerMetaTypes();
        QDBusInterface service(kService, kServicePath, kServiceIface, bus_);
        QDBusMessage reply = service.call(
            QStringLiteral("OpenSession"), QStringLiteral("plain"),
            QVariant::fromValue(QDBusVariant(QString())));
        if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().size() < 2) {
            if (error != nullptr)
                *error = QStringLiteral("secret service: OpenSession failed: %1")
                             .arg(reply.errorMessage());
            return false;
        }
        session_ = reply.arguments().at(1).value<QDBusObjectPath>();

        collection_ = resolveCollection(error);
        if (collection_.path().isEmpty()) return false;

        // Разблокировать связку, если заперта (обычно открыта с входом в
        // сессию).
        QDBusMessage unlockReply = service.call(
            QStringLiteral("Unlock"),
            QVariant::fromValue(QList<QDBusObjectPath>{collection_}));
        if (unlockReply.type() == QDBusMessage::ReplyMessage &&
            unlockReply.arguments().size() >= 2) {
            const auto prompt = unlockReply.arguments().at(1).value<QDBusObjectPath>();
            if (!completePrompt(prompt, error)) return false;
        }
        return true;
    }

    // Запись-свёрток; unlockIfNeeded — отпереть найденную запертую (это уже
    // явный жест чтения/записи). Пустой путь + пустой error — записи нет.
    QDBusObjectPath findBundle(bool unlockIfNeeded, QString* error) {
        QDBusInterface service(kService, kServicePath, kServiceIface, bus_);
        QDBusMessage reply = service.call(QStringLiteral("SearchItems"),
                                          QVariant::fromValue(bundleAttrs()));
        if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().size() < 2) {
            if (error != nullptr)
                *error = QStringLiteral("secret service: SearchItems failed: %1")
                             .arg(reply.errorMessage());
            return {};
        }
        const auto unlocked =
            qdbus_cast<QList<QDBusObjectPath>>(reply.arguments().at(0));
        const auto locked =
            qdbus_cast<QList<QDBusObjectPath>>(reply.arguments().at(1));
        if (!unlocked.isEmpty()) return unlocked.first();
        if (locked.isEmpty() || !unlockIfNeeded) return {};
        QDBusMessage unlockReply = service.call(QStringLiteral("Unlock"),
                                                QVariant::fromValue(locked));
        if (unlockReply.type() == QDBusMessage::ReplyMessage &&
            unlockReply.arguments().size() >= 2 &&
            completePrompt(unlockReply.arguments().at(1).value<QDBusObjectPath>(),
                           error))
            return locked.first();
        return {};
    }

    QByteArray getSecret(const QDBusObjectPath& item, QString* error) {
        QDBusInterface itemIface(kService, item.path(), kItemIface, bus_);
        QDBusMessage reply = itemIface.call(QStringLiteral("GetSecret"),
                                            QVariant::fromValue(session_));
        if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty()) {
            if (error != nullptr)
                *error = QStringLiteral("secret service: GetSecret failed: %1")
                             .arg(reply.errorMessage());
            return {};
        }
        return qdbus_cast<DBusSecret>(reply.arguments().at(0)).value;
    }

    // --- переезд с записей-на-секрет ---------------------------------------

    bool migrateLegacy(QByteArray* data, bool* found, QString* error) {
        // Все наши записи; свёрток сюда не попадёт (его нет — потому мы тут),
        // а чужие «application: zametti» без what пропускаются по атрибутам.
        QDBusInterface service(kService, kServicePath, kServiceIface, bus_);
        QDBusMessage reply = service.call(
            QStringLiteral("SearchItems"),
            QVariant::fromValue(QMap<QString, QString>{
                {QStringLiteral("application"), QStringLiteral("zametti")}}));
        if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().size() < 2)
            return true;   // связки не разглядеть — пусть свёртка просто нет
        QList<QDBusObjectPath> items =
            qdbus_cast<QList<QDBusObjectPath>>(reply.arguments().at(0));
        items += qdbus_cast<QList<QDBusObjectPath>>(reply.arguments().at(1));
        if (items.isEmpty()) return true;   // чистая связка — не миграция

        SecretBundle moved;
        QList<QDBusObjectPath> emptied;
        for (const QDBusObjectPath& item : items) {
            QDBusInterface props(kService, item.path(), kPropertiesIface, bus_);
            QDBusReply<QDBusVariant> got = props.call(QStringLiteral("Get"),
                                                      QLatin1String(kItemIface),
                                                      QStringLiteral("Attributes"));
            if (!got.isValid()) continue;
            const auto attrs =
                qdbus_cast<QMap<QString, QString>>(got.value().variant());
            const QString storeId = attrs.value(QStringLiteral("storeId"));
            const QString what = attrs.value(QStringLiteral("what"));
            SecretStore::Secret slot;
            if (what == QLatin1String("key"))
                slot = SecretStore::Secret::Key;
            else if (what == QLatin1String("webdav"))
                slot = SecretStore::Secret::ServerPassword;
            else if (what == QLatin1String("password"))
                slot = SecretStore::Secret::EncryptionPassword;
            else
                continue;   // не запись прежнего формата — не трогаем
            if (storeId.isEmpty()) continue;
            QString readError;
            const QByteArray value = getSecret(item, &readError);
            if (!readError.isEmpty()) {
                // Непрочитанное не удаляется и не теряется: переезд честно
                // откладывается целиком до следующего чтения.
                if (error != nullptr) *error = readError;
                return false;
            }
            moved.put(storeId, slot, value);
            emptied.append(item);
        }
        if (moved.isEmpty()) return true;

        const QByteArray bytes = moved.toBytes();
        if (!writeBundle(bytes, moved.index(), error)) return false;
        // Только после удачной записи: старые записи, оставленные жить, лгали
        // бы витрине связки и воскресали бы при откате версии программы.
        for (const QDBusObjectPath& item : emptied) {
            QDBusInterface itemIface(kService, item.path(), kItemIface, bus_);
            QDBusMessage del = itemIface.call(QStringLiteral("Delete"));
            if (del.type() == QDBusMessage::ReplyMessage && !del.arguments().isEmpty())
                completePrompt(del.arguments().at(0).value<QDBusObjectPath>(), nullptr);
            else
                qWarning("keyring: legacy item %s was not deleted",
                         qPrintable(item.path()));
        }
        *data = bytes;
        *found = true;
        return true;
    }

    QDBusConnection bus_ = QDBusConnection::sessionBus();
    QDBusObjectPath session_;      // пустой путь — сессия ещё не открыта
    QDBusObjectPath collection_;
};

}  // namespace

// KeyringSecrets — общий фасад трёх систем (заголовок один); на Linux под ним
// живёт DbusBundle.
struct KeyringSecrets::Impl {
    DbusBundle bundle;
};

KeyringSecrets::KeyringSecrets() : impl_(std::make_shared<Impl>()) {}
KeyringSecrets::~KeyringSecrets() = default;

bool KeyringSecrets::available() const { return impl_->bundle.available(); }

bool KeyringSecrets::has(const QString& storeId, Secret which) {
    return impl_->bundle.has(storeId, which);
}

bool KeyringSecrets::loadKey(const QString& storeId, Keyfile* out, QString* error) {
    return impl_->bundle.loadKey(storeId, out, error);
}

bool KeyringSecrets::storeKey(const Keyfile& keyfile, QString* error) {
    return impl_->bundle.storeKey(keyfile, error);
}

bool KeyringSecrets::clearKey(const QString& storeId, QString* error) {
    return impl_->bundle.clearKey(storeId, error);
}

QString KeyringSecrets::serverPassword(const QString& storeId, QString* error) {
    return impl_->bundle.serverPassword(storeId, error);
}

bool KeyringSecrets::setServerPassword(const QString& storeId,
                                       const QString& password, QString* error) {
    return impl_->bundle.setServerPassword(storeId, password, error);
}

bool KeyringSecrets::clearServerPassword(const QString& storeId, QString* error) {
    return impl_->bundle.clearServerPassword(storeId, error);
}

QString KeyringSecrets::encryptionPassword(const QString& storeId, QString* error) {
    return impl_->bundle.encryptionPassword(storeId, error);
}

bool KeyringSecrets::setEncryptionPassword(const QString& storeId,
                                           const QString& password, QString* error) {
    return impl_->bundle.setEncryptionPassword(storeId, password, error);
}

bool KeyringSecrets::clearEncryptionPassword(const QString& storeId, QString* error) {
    return impl_->bundle.clearEncryptionPassword(storeId, error);
}

}  // namespace zametti
