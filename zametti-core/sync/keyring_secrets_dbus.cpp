// KeyringSecrets на Linux: Secret Service API по QDBus.
//
// Протокол (freedesktop.org/wiki/Specifications/secret-storage-spec):
// OpenSession → коллекция по алиасу default → Unlock (с Prompt, если демон
// спросит) → SearchItems по атрибутам / CreateItem (replace) / GetSecret /
// Delete. Атрибуты — {application: zametti, storeId, what: key|webdav}:
// по ним элемент ищется, метка — только витрина для человека.

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

QMap<QString, QString> attributesFor(const QString& storeId, const char* what) {
    return {{QStringLiteral("application"), QStringLiteral("zametti")},
            {QStringLiteral("storeId"), storeId},
            {QStringLiteral("what"), QLatin1String(what)}};
}

const char* whatFor(zametti::SecretStore::Secret which) {
    switch (which) {
        case zametti::SecretStore::Secret::Key: return "key";
        case zametti::SecretStore::Secret::ServerPassword: return "webdav";
        case zametti::SecretStore::Secret::EncryptionPassword: return "password";
    }
    return "";
}

}  // namespace
}  // namespace zametti

// Q_DECLARE_METATYPE — только в глобальной области видимости.
Q_DECLARE_METATYPE(zametti::DBusSecret)

namespace zametti {

struct KeyringSecrets::Impl {
    QDBusConnection bus = QDBusConnection::sessionBus();
    QDBusObjectPath session;       // пустой путь — сессия ещё не открыта
    QDBusObjectPath collection;

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
        bus.connect(kService, prompt.path(), kPromptIface,
                    QStringLiteral("Completed"), &loop, SLOT(quit()));
        QDBusInterface iface(kService, prompt.path(), kPromptIface, bus);
        QDBusReply<void> shown = iface.call(QStringLiteral("Prompt"), QString());
        if (!shown.isValid()) {
            if (error != nullptr) *error = shown.error().message();
            return false;
        }
        QTimer::singleShot(60000, &loop, &QEventLoop::quit);
        loop.exec();
        return true;
    }

    bool ensureSession(QString* error) {
        if (!session.path().isEmpty()) return true;
        if (!bus.isConnected()) {
            if (error != nullptr) *error = QStringLiteral("no D-Bus session bus");
            return false;
        }
        qDBusRegisterMetaType<DBusSecret>();
        qDBusRegisterMetaType<QMap<QString, QString>>();   // a{ss} — атрибуты
        QDBusInterface service(kService, kServicePath, kServiceIface, bus);
        QDBusMessage reply = service.call(
            QStringLiteral("OpenSession"), QStringLiteral("plain"),
            QVariant::fromValue(QDBusVariant(QString())));
        if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().size() < 2) {
            if (error != nullptr)
                *error = QStringLiteral("secret service: OpenSession failed: %1")
                             .arg(reply.errorMessage());
            return false;
        }
        session = reply.arguments().at(1).value<QDBusObjectPath>();

        // Коллекция по алиасу default; "/" — связки нет вовсе.
        QDBusMessage aliasReply = service.call(QStringLiteral("ReadAlias"),
                                               QStringLiteral("default"));
        if (aliasReply.type() != QDBusMessage::ReplyMessage ||
            aliasReply.arguments().isEmpty()) {
            if (error != nullptr)
                *error = QStringLiteral("secret service: ReadAlias failed");
            return false;
        }
        collection = aliasReply.arguments().at(0).value<QDBusObjectPath>();
        if (collection.path() == QLatin1String("/") || collection.path().isEmpty()) {
            if (error != nullptr)
                *error = QStringLiteral("secret service: no default collection");
            return false;
        }
        // Разблокировать связку, если заперта (обычно открыта с входом в
        // сессию).
        QDBusMessage unlockReply = service.call(
            QStringLiteral("Unlock"),
            QVariant::fromValue(QList<QDBusObjectPath>{collection}));
        if (unlockReply.type() == QDBusMessage::ReplyMessage &&
            unlockReply.arguments().size() >= 2) {
            const auto prompt = unlockReply.arguments().at(1).value<QDBusObjectPath>();
            if (!completePrompt(prompt, error)) return false;
        }
        return true;
    }

    // Первый найденный элемент с такими атрибутами; пусто — нет.
    QDBusObjectPath findItem(const QMap<QString, QString>& attrs, QString* error) {
        QDBusInterface service(kService, kServicePath, kServiceIface, bus);
        QDBusMessage reply = service.call(QStringLiteral("SearchItems"),
                                          QVariant::fromValue(attrs));
        if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().size() < 2) {
            if (error != nullptr)
                *error = QStringLiteral("secret service: SearchItems failed: %1")
                             .arg(reply.errorMessage());
            return QDBusObjectPath();
        }
        auto unlocked = qdbus_cast<QList<QDBusObjectPath>>(reply.arguments().at(0));
        auto locked = qdbus_cast<QList<QDBusObjectPath>>(reply.arguments().at(1));
        if (!unlocked.isEmpty()) return unlocked.first();
        if (!locked.isEmpty()) {
            QDBusMessage unlockReply = service.call(
                QStringLiteral("Unlock"), QVariant::fromValue(locked));
            if (unlockReply.type() == QDBusMessage::ReplyMessage &&
                unlockReply.arguments().size() >= 2 &&
                completePrompt(unlockReply.arguments().at(1).value<QDBusObjectPath>(),
                               error))
                return locked.first();
        }
        return QDBusObjectPath();
    }

    bool putSecret(const QString& label, const QMap<QString, QString>& attrs,
                   const QByteArray& value, const char* contentType,
                   QString* error) {
        if (!ensureSession(error)) return false;
        // Attributes в свойствах едут как a{ss} (метатип выше).
        QVariantMap properties;
        properties.insert(QStringLiteral("org.freedesktop.Secret.Item.Label"), label);
        properties.insert(QStringLiteral("org.freedesktop.Secret.Item.Attributes"),
                          QVariant::fromValue(attrs));
        DBusSecret secret;
        secret.session = session;
        secret.value = value;
        secret.contentType = QLatin1String(contentType);

        QDBusInterface coll(kService, collection.path(), kCollectionIface, bus);
        QDBusMessage reply = coll.call(QStringLiteral("CreateItem"),
                                       QVariant::fromValue(properties),
                                       QVariant::fromValue(secret),
                                       true /* replace */);
        if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().size() < 2) {
            if (error != nullptr)
                *error = QStringLiteral("secret service: CreateItem failed: %1")
                             .arg(reply.errorMessage());
            return false;
        }
        return completePrompt(reply.arguments().at(1).value<QDBusObjectPath>(), error);
    }

    QByteArray getSecret(const QMap<QString, QString>& attrs, bool* found,
                         QString* error) {
        *found = false;
        if (!ensureSession(error)) return QByteArray();
        const QDBusObjectPath item = findItem(attrs, error);
        if (item.path().isEmpty()) return QByteArray();
        QDBusInterface itemIface(kService, item.path(), kItemIface, bus);
        QDBusMessage reply = itemIface.call(QStringLiteral("GetSecret"),
                                            QVariant::fromValue(session));
        if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty()) {
            if (error != nullptr)
                *error = QStringLiteral("secret service: GetSecret failed: %1")
                             .arg(reply.errorMessage());
            return QByteArray();
        }
        const auto secret = qdbus_cast<DBusSecret>(reply.arguments().at(0));
        *found = true;
        return secret.value;
    }

    bool removeSecret(const QMap<QString, QString>& attrs, QString* error) {
        if (!ensureSession(error)) return false;
        const QDBusObjectPath item = findItem(attrs, error);
        if (item.path().isEmpty()) return true;   // нечего убирать — не беда
        QDBusInterface itemIface(kService, item.path(), kItemIface, bus);
        QDBusMessage reply = itemIface.call(QStringLiteral("Delete"));
        if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty()) {
            if (error != nullptr)
                *error = QStringLiteral("secret service: Delete failed: %1")
                             .arg(reply.errorMessage());
            return false;
        }
        return completePrompt(reply.arguments().at(0).value<QDBusObjectPath>(), error);
    }
};

KeyringSecrets::KeyringSecrets() : impl_(std::make_shared<Impl>()) {}
KeyringSecrets::~KeyringSecrets() = default;

bool KeyringSecrets::available() const {
    if (!impl_->bus.isConnected()) return false;
    const QDBusConnectionInterface* iface = impl_->bus.interface();
    if (iface == nullptr) return false;
    // Служба либо уже на шине, либо активируется по обращению.
    if (iface->isServiceRegistered(QLatin1String(kService))) return true;
    const auto activatable = iface->activatableServiceNames();
    return activatable.isValid() &&
           activatable.value().contains(QLatin1String(kService));
}

bool KeyringSecrets::has(const QString& storeId, Secret which) {
    // СУЩЕСТВОВАНИЕ, А НЕ ЧТЕНИЕ: SearchItems без GetSecret и — в отличие от
    // findItem — БЕЗ Unlock: запертая связка не повод будить человека вопросом,
    // элемент виден и в списке locked. Сессия не нужна (SearchItems — метод
    // службы), нужен только метатип атрибутов.
    if (!impl_->bus.isConnected()) return false;
    qDBusRegisterMetaType<QMap<QString, QString>>();
    QDBusInterface service(kService, kServicePath, kServiceIface, impl_->bus);
    QDBusMessage reply = service.call(
        QStringLiteral("SearchItems"),
        QVariant::fromValue(attributesFor(storeId, whatFor(which))));
    if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().size() < 2)
        return false;
    const auto unlocked =
        qdbus_cast<QList<QDBusObjectPath>>(reply.arguments().at(0));
    const auto locked = qdbus_cast<QList<QDBusObjectPath>>(reply.arguments().at(1));
    return !unlocked.isEmpty() || !locked.isEmpty();
}

bool KeyringSecrets::loadKey(const QString& storeId, Keyfile* out, QString* error) {
    Q_ASSERT(out != nullptr);
    bool found = false;
    const QByteArray bytes =
        impl_->getSecret(attributesFor(storeId, "key"), &found, error);
    if (!found) {
        if (error != nullptr && error->isEmpty())
            *error = QStringLiteral("no key for %1 in the keyring").arg(storeId);
        return false;
    }
    const Keyfile made = Keyfile::fromLiveKey(storeId, bytes);
    if (!made.hasKey()) {
        if (error != nullptr)
            *error = QStringLiteral("keyring item for %1 is not a %2-byte key")
                         .arg(storeId).arg(Keyfile::kKeyBytes);
        return false;
    }
    *out = made;
    return true;
}

bool KeyringSecrets::storeKey(const Keyfile& keyfile, QString* error) {
    if (!keyfile.hasKey()) {
        if (error != nullptr) *error = QStringLiteral("no live key to store");
        return false;
    }
    return impl_->putSecret(
        QStringLiteral("zametti-key-%1").arg(keyfile.storeId()),
        attributesFor(keyfile.storeId(), "key"), keyfile.key(),
        "application/octet-stream", error);
}

bool KeyringSecrets::clearKey(const QString& storeId, QString* error) {
    return impl_->removeSecret(attributesFor(storeId, "key"), error);
}

QString KeyringSecrets::serverPassword(const QString& storeId, QString* error) {
    bool found = false;
    const QByteArray bytes =
        impl_->getSecret(attributesFor(storeId, "webdav"), &found, error);
    if (!found) {
        if (error != nullptr && error->isEmpty())
            *error = QStringLiteral("no server password for %1 in the keyring")
                         .arg(storeId);
        return QString();
    }
    return QString::fromUtf8(bytes);
}

bool KeyringSecrets::setServerPassword(const QString& storeId,
                                       const QString& password, QString* error) {
    return impl_->putSecret(
        QStringLiteral("zametti-webdav-%1").arg(storeId),
        attributesFor(storeId, "webdav"), password.toUtf8(),
        "text/plain; charset=utf8", error);
}

bool KeyringSecrets::clearServerPassword(const QString& storeId, QString* error) {
    return impl_->removeSecret(attributesFor(storeId, "webdav"), error);
}

QString KeyringSecrets::encryptionPassword(const QString& storeId, QString* error) {
    bool found = false;
    const QByteArray bytes =
        impl_->getSecret(attributesFor(storeId, "password"), &found, error);
    if (!found) {
        if (error != nullptr && error->isEmpty())
            *error = QStringLiteral("no encryption password for %1 in the keyring")
                         .arg(storeId);
        return QString();
    }
    return QString::fromUtf8(bytes);
}

bool KeyringSecrets::setEncryptionPassword(const QString& storeId,
                                           const QString& password, QString* error) {
    return impl_->putSecret(
        QStringLiteral("zametti-password-%1").arg(storeId),
        attributesFor(storeId, "password"), password.toUtf8(),
        "text/plain; charset=utf8", error);
}

bool KeyringSecrets::clearEncryptionPassword(const QString& storeId, QString* error) {
    return impl_->removeSecret(attributesFor(storeId, "password"), error);
}

}  // namespace zametti
