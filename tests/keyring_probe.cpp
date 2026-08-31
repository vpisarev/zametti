// Ручная приёмка keyring (m17, сессия 3): записать / прочитать / удалить под
// живым Secret Service (GNOME Keyring, KWallet).
//
// Это НЕ набор: живой keyring — вещь машины, не проверки, и гонять его в
// zametti-tests значило бы гадить в связку пользователя при каждом прогоне.
// Пробник печатает каждый шаг; САМИ СЕКРЕТЫ НЕ ПЕЧАТАЕТ.
//
//   zametti-bench keyring [--store-id <id>]
//
// Работает на выдуманном storeId (дефолт ниже) и подчищает за собой.

#include "blob_cipher.h"
#include "keyring_secrets.h"

#include <cstdio>
#include <cstring>

#ifdef Q_OS_MACOS
#include <Security/Security.h>
#endif

#if defined(Q_OS_UNIX) && !defined(Q_OS_MACOS)
#define ZT_PROBE_DBUS 1
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusMetaType>
#include <QDBusObjectPath>
#include <QDBusReply>
#include <QDBusVariant>
#endif

int ztKeyringProbe(int argc, char** argv);

namespace {

int fail(const char* what, const QString& error) {
    std::printf("ПРОВАЛ: %s: %s\n", what, error.toUtf8().constData());
    return 1;
}

#ifdef Q_OS_MACOS

// ИЗМЕРЕНИЕ ДЛЯ МАКА (вопрос §1.2 матрицы окна хранилищ): поднимает ли чтение
// ОДНИХ АТРИБУТОВ (kSecReturnAttributes без kSecReturnData) системный вопрос
// связки. От ответа зависит право показа спрашивать «есть ли секрет», не читая
// его. Ответ видно только глазами на экране — пробник печатает, КОГДА смотреть.
//
// Работает по сырым SecItem* и ТОЛЬКО с записью пробника
// (service io.zametti.zametti.probe) — боевую запись не трогает вовсе.

void cfRelease(CFTypeRef ref) {
    if (ref != nullptr) CFRelease(ref);
}

QString osStatusText(OSStatus status) {
    CFStringRef text = SecCopyErrorMessageString(status, nullptr);
    const QString out = text != nullptr ? QString::fromCFString(text)
                                        : QStringLiteral("OSStatus %1").arg(status);
    cfRelease(text);
    return out;
}

// База запроса: класс + service + account. Всё остальное шаги добавляют сами.
CFMutableDictionaryRef probeQuery() {
    CFMutableDictionaryRef q = CFDictionaryCreateMutable(
        nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(q, kSecClass, kSecClassGenericPassword);
    CFDictionarySetValue(q, kSecAttrService, CFSTR("io.zametti.zametti.probe"));
    CFDictionarySetValue(q, kSecAttrAccount, CFSTR("secrets"));
    return q;
}

int measureKeychain(bool keep) {
    std::printf("\n== измерение связки (сырые SecItem*, запись пробника) ==\n");
    std::printf("КАЖДЫЙ ШАГ: следите за экраном — был ли системный вопрос.\n\n");

    const QByteArray blob = QByteArrayLiteral("probe-bundle-bytes");
    const QByteArray index = QByteArrayLiteral("probe-index-bytes");

    // Запись от ПРЕЖНЕГО прогона не убирается, а ЧИТАЕТСЯ: если тот бинарь был
    // другой сборки, чтение данных ниже — это ровно сценарий «после пересборки»
    // (свой процесс, как выяснил первый замер, читает без вопросов; вопрос
    // возникает при чтении чужой записи — слова владельца, сверенные замером).
    bool existed = false;
    {
        CFMutableDictionaryRef q = probeQuery();
        CFDictionarySetValue(q, kSecReturnAttributes, kCFBooleanTrue);
        CFTypeRef out = nullptr;
        const OSStatus st = SecItemCopyMatching(q, &out);
        cfRelease(q);
        cfRelease(out);
        if (st == errSecSuccess) existed = true;
        else if (st != errSecItemNotFound)
            return fail("SecItemCopyMatching(есть ли)", osStatusText(st));
    }

    // 1. Создать запись, если её нет: данные + индекс в kSecAttrGeneric.
    if (!existed) {
        CFMutableDictionaryRef add = probeQuery();
        CFDataRef data = CFDataCreate(nullptr, (const UInt8*)blob.constData(),
                                      blob.size());
        CFDataRef gen = CFDataCreate(nullptr, (const UInt8*)index.constData(),
                                     index.size());
        CFDictionarySetValue(add, kSecValueData, data);
        CFDictionarySetValue(add, kSecAttrGeneric, gen);
        CFDictionarySetValue(add, kSecAttrLabel, CFSTR("io.zametti.zametti.probe"));
        const OSStatus st = SecItemAdd(add, nullptr);
        cfRelease(gen);
        cfRelease(data);
        cfRelease(add);
        if (st != errSecSuccess)
            return fail("SecItemAdd", osStatusText(st));
        std::printf("шаг 1, SecItemAdd: запись создана. Был ли вопрос?\n");
    } else {
        std::printf("шаг 1: запись УЖЕ ЕСТЬ (оставлена прежним прогоном) — "
                    "читаем её; если тот бинарь был другой сборки, это замер "
                    "«после пересборки».\n");
    }

    // 2. ГЛАВНЫЙ ШАГ: одни атрибуты, БЕЗ данных.
    {
        CFMutableDictionaryRef q = probeQuery();
        CFDictionarySetValue(q, kSecReturnAttributes, kCFBooleanTrue);
        CFTypeRef out = nullptr;
        const OSStatus st = SecItemCopyMatching(q, &out);
        cfRelease(q);
        if (st != errSecSuccess)
            return fail("SecItemCopyMatching(атрибуты)", osStatusText(st));
        const auto attrs = (CFDictionaryRef)out;
        const auto gen = (CFDataRef)CFDictionaryGetValue(attrs, kSecAttrGeneric);
        const bool indexBack =
            gen != nullptr &&
            QByteArray((const char*)CFDataGetBytePtr(gen),
                       int(CFDataGetLength(gen))) == index;
        cfRelease(out);
        if (!indexBack)
            return fail("kSecAttrGeneric", QStringLiteral(
                            "индекс не вернулся из атрибутов"));
        std::printf("шаг 2, ОДНИ АТРИБУТЫ: индекс прочитан без данных. "
                    "ГЛАВНОЕ: был ли вопрос?\n");
    }

    // 3. Данные (kSecReturnData) — на ЧУЖОЙ записи вопрос ожидаем здесь.
    {
        CFMutableDictionaryRef q = probeQuery();
        CFDictionarySetValue(q, kSecReturnData, kCFBooleanTrue);
        CFTypeRef out = nullptr;
        const OSStatus st = SecItemCopyMatching(q, &out);
        cfRelease(q);
        if (st != errSecSuccess)
            return fail("SecItemCopyMatching(данные)", osStatusText(st));
        const QByteArray got((const char*)CFDataGetBytePtr((CFDataRef)out),
                             qsizetype(CFDataGetLength((CFDataRef)out)));
        cfRelease(out);
        // Свежая запись держит байты шага 1, оставленная прежним прогоном —
        // байты его шага 4 (обновление): законны оба содержимых. Первый
        // прогон замера «после пересборки» спотыкался ровно об это.
        if (got != blob && got != QByteArrayLiteral("probe-bundle-2"))
            return fail("kSecValueData", QStringLiteral("данные вернулись не те"));
        std::printf("шаг 3, ДАННЫЕ: прочитаны и совпали. Был ли вопрос?\n");
    }

    // 4. Обновить данные и индекс НЕ ПЕРЕСОЗДАВАЯ запись (политика владельца:
    //    пересоздание сбрасывает ACL и плодит вопросы).
    {
        CFMutableDictionaryRef q = probeQuery();
        CFMutableDictionaryRef upd = CFDictionaryCreateMutable(
            nullptr, 0, &kCFTypeDictionaryKeyCallBacks,
            &kCFTypeDictionaryValueCallBacks);
        const QByteArray blob2 = QByteArrayLiteral("probe-bundle-2");
        CFDataRef data = CFDataCreate(nullptr, (const UInt8*)blob2.constData(),
                                      blob2.size());
        CFDictionarySetValue(upd, kSecValueData, data);
        const OSStatus st = SecItemUpdate(q, upd);
        cfRelease(data);
        cfRelease(upd);
        cfRelease(q);
        if (st != errSecSuccess)
            return fail("SecItemUpdate", osStatusText(st));
        std::printf("шаг 4, SecItemUpdate: обновлено без пересоздания. "
                    "Был ли вопрос?\n");
    }

    // 5. Убрать за собой — если не попросили оставить (--keep оставляет запись
    //    следующему прогону: пересобрал бинарь → чтение стало «чужим»).
    if (!keep) {
        CFMutableDictionaryRef q = probeQuery();
        const OSStatus st = SecItemDelete(q);
        cfRelease(q);
        if (st != errSecSuccess)
            return fail("SecItemDelete", osStatusText(st));
        std::printf("шаг 5, SecItemDelete: запись пробника убрана.\n");
    } else {
        std::printf("шаг 5: --keep — запись ОСТАВЛЕНА для замера «после "
                    "пересборки» (уберёт следующий прогон без --keep).\n");
    }

    std::printf("== измерение пройдено; ответы — у того, кто смотрел ==\n\n");
    return 0;
}

#endif  // Q_OS_MACOS

#ifdef ZT_PROBE_DBUS

// ИЗМЕРЕНИЯ ДЛЯ LINUX (Secret Service, 31.08.2026). Три вопроса:
//
//   1. Видна ли связка и заперта ли она (available() отвечает «нет» на
//      запертую — это надо видеть глазами вместе с причиной).
//   2. Читаются ли АТРИБУТЫ item'а у ЗАПЕРТОЙ коллекции — спецификация этого
//      не обещает (открытая часть файла gnome-keyring несёт хеши атрибутов);
//      от ответа зависит, может ли has() отвечать по индексу без Unlock.
//      Меряется ТОЛЬКО если коллекция заперта на входе: запирать связку
//      владельца пробник не смеет.
//   3. Миграция запись-на-секрет → свёрток: пробник кладёт СЫРЫМИ вызовами
//      legacy-записи на выдуманном storeId, дёргает чтение через
//      KeyringSecrets и смотрит, что записи собрались в свёрток, а сами
//      исчезли. Подчищает за собой слоты свёртка.

struct ProbeSecret {
    QDBusObjectPath session;
    QByteArray parameters;
    QByteArray value;
    QString contentType;
};

QDBusArgument& operator<<(QDBusArgument& arg, const ProbeSecret& secret) {
    arg.beginStructure();
    arg << secret.session << secret.parameters << secret.value << secret.contentType;
    arg.endStructure();
    return arg;
}

const QDBusArgument& operator>>(const QDBusArgument& arg, ProbeSecret& secret) {
    arg.beginStructure();
    arg >> secret.session >> secret.parameters >> secret.value >> secret.contentType;
    arg.endStructure();
    return arg;
}

#endif  // ZT_PROBE_DBUS

}  // namespace

#ifdef ZT_PROBE_DBUS
Q_DECLARE_METATYPE(ProbeSecret)
#endif

namespace {

#ifdef ZT_PROBE_DBUS

constexpr char kSs[] = "org.freedesktop.secrets";
constexpr char kSsPath[] = "/org/freedesktop/secrets";
constexpr char kSsIface[] = "org.freedesktop.Secret.Service";
constexpr char kSsProps[] = "org.freedesktop.DBus.Properties";
constexpr char kSsItem[] = "org.freedesktop.Secret.Item";

QVariant ssProperty(const QString& path, const char* iface, const QString& name) {
    QDBusInterface props(kSs, path, kSsProps, QDBusConnection::sessionBus());
    QDBusReply<QDBusVariant> got =
        props.call(QStringLiteral("Get"), QLatin1String(iface), name);
    if (!got.isValid()) return {};
    return got.value().variant();
}

QList<QDBusObjectPath> ssSearch(const QMap<QString, QString>& attrs, bool* lockedToo) {
    QDBusInterface service(kSs, kSsPath, kSsIface, QDBusConnection::sessionBus());
    QDBusMessage reply = service.call(QStringLiteral("SearchItems"),
                                      QVariant::fromValue(attrs));
    QList<QDBusObjectPath> out;
    if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().size() < 2)
        return out;
    out = qdbus_cast<QList<QDBusObjectPath>>(reply.arguments().at(0));
    const auto locked = qdbus_cast<QList<QDBusObjectPath>>(reply.arguments().at(1));
    if (lockedToo != nullptr) *lockedToo = !locked.isEmpty();
    out += locked;
    return out;
}

// Заперта ли коллекция default (или login, если алиас молчит); -1 — не видно.
int ssLockedState(QString* collPath) {
    QDBusInterface service(kSs, kSsPath, kSsIface, QDBusConnection::sessionBus());
    QDBusMessage aliasReply =
        service.call(QStringLiteral("ReadAlias"), QStringLiteral("default"));
    QString path;
    if (aliasReply.type() == QDBusMessage::ReplyMessage &&
        !aliasReply.arguments().isEmpty())
        path = aliasReply.arguments().at(0).value<QDBusObjectPath>().path();
    if (path.isEmpty() || path == QLatin1String("/"))
        path = QStringLiteral("/org/freedesktop/secrets/collection/login");
    if (collPath != nullptr) *collPath = path;
    const QVariant locked =
        ssProperty(path, "org.freedesktop.Secret.Collection", QStringLiteral("Locked"));
    if (!locked.isValid()) return -1;
    return locked.toBool() ? 1 : 0;
}

int measureSecretService(const QString& storeId) {
    std::printf("\n== измерение Secret Service ==\n");
    qDBusRegisterMetaType<ProbeSecret>();
    qDBusRegisterMetaType<QMap<QString, QString>>();

    QString coll;
    const int locked = ssLockedState(&coll);
    if (locked < 0) {
        std::printf("коллекция не видна вовсе (ReadAlias/Properties молчат) — "
                    "дальше мерить нечего\n");
        return 0;
    }
    std::printf("коллекция: %s, %s\n", coll.toUtf8().constData(),
                locked == 1 ? "ЗАПЕРТА" : "отперта");

    // Вопрос 2: атрибуты у запертой коллекции.
    if (locked == 1) {
        bool lockedFound = false;
        const auto items = ssSearch({{QStringLiteral("application"),
                                      QStringLiteral("zametti")}},
                                    &lockedFound);
        if (items.isEmpty()) {
            std::printf("замер «атрибуты у запертой»: НЕ ИЗМЕРЕНО — в связке нет "
                        "ни одной записи zametti\n");
        } else {
            const QVariant attrs = ssProperty(items.first().path(), kSsItem,
                                              QStringLiteral("Attributes"));
            const auto map = qdbus_cast<QMap<QString, QString>>(attrs);
            std::printf("замер «атрибуты у запертой»: SearchItems видит %d зап., "
                        "Properties.Get(Attributes) %s (ключей: %d)\n",
                        int(items.size()),
                        attrs.isValid() ? "ОТВЕТИЛ" : "ПРОМОЛЧАЛ",
                        int(map.size()));
        }
        std::printf("связка заперта: дальнейшая приёмка (запись/чтение/миграция) "
                    "требует отпертой связки — Unlock пробник сам не зовёт.\n");
        std::printf("== измерение окончено ==\n\n");
        return 0;
    }

    // Вопрос 3 (связка отперта): миграция legacy → свёрток на выдуманном id.
    // Сырые CreateItem: сессия plain, replace=true (это пробные записи).
    QDBusInterface service(kSs, kSsPath, kSsIface, QDBusConnection::sessionBus());
    QDBusMessage open = service.call(QStringLiteral("OpenSession"),
                                     QStringLiteral("plain"),
                                     QVariant::fromValue(QDBusVariant(QString())));
    if (open.type() != QDBusMessage::ReplyMessage || open.arguments().size() < 2)
        return fail("OpenSession", open.errorMessage());
    const auto session = open.arguments().at(1).value<QDBusObjectPath>();

    const auto plant = [&](const char* what, const QByteArray& value) -> bool {
        QVariantMap properties;
        properties.insert(QStringLiteral("org.freedesktop.Secret.Item.Label"),
                          QStringLiteral("zametti-%1-%2")
                              .arg(QLatin1String(what), storeId));
        properties.insert(
            QStringLiteral("org.freedesktop.Secret.Item.Attributes"),
            QVariant::fromValue(QMap<QString, QString>{
                {QStringLiteral("application"), QStringLiteral("zametti")},
                {QStringLiteral("storeId"), storeId},
                {QStringLiteral("what"), QLatin1String(what)}}));
        ProbeSecret secret;
        secret.session = session;
        secret.value = value;
        secret.contentType = QStringLiteral("application/octet-stream");
        QDBusInterface coll_(kSs, coll, "org.freedesktop.Secret.Collection",
                             QDBusConnection::sessionBus());
        QDBusMessage made = coll_.call(QStringLiteral("CreateItem"),
                                       QVariant::fromValue(properties),
                                       QVariant::fromValue(secret), true);
        return made.type() == QDBusMessage::ReplyMessage;
    };
    if (!plant("webdav", QByteArrayLiteral("проба-миграции")))
        return fail("CreateItem(legacy)", QStringLiteral("не создалась"));
    std::printf("миграция: legacy-запись webdav посажена сырым CreateItem\n");

    {
        zametti::KeyringSecrets fresh;
        QString error;
        const QString got = fresh.serverPassword(storeId, &error);
        if (got != QStringLiteral("проба-миграции"))
            return fail("миграция: чтение вернуло не то", error);
        std::printf("миграция: чтение через KeyringSecrets вернуло секрет "
                    "(свёрток собран)\n");

        const auto leftovers = ssSearch({{QStringLiteral("application"),
                                          QStringLiteral("zametti")},
                                         {QStringLiteral("storeId"), storeId}},
                                        nullptr);
        if (!leftovers.isEmpty())
            return fail("миграция: legacy-записи остались",
                        QString::number(leftovers.size()));
        std::printf("миграция: legacy-записи исчезли\n");

        const auto bundles = ssSearch({{QStringLiteral("application"),
                                        QStringLiteral("zametti")},
                                       {QStringLiteral("kind"),
                                        QStringLiteral("bundle")}},
                                      nullptr);
        if (bundles.size() != 1)
            return fail("миграция: записей-свёртков не одна",
                        QString::number(bundles.size()));
        std::printf("миграция: запись-свёрток ровно одна\n");

        if (!fresh.has(storeId, zametti::SecretStore::Secret::ServerPassword))
            return fail("has() после миграции",
                        QStringLiteral("индекс не видит пароль"));
        std::printf("миграция: has() отвечает по индексу\n");

        if (!fresh.clearServerPassword(storeId, &error))
            return fail("подчистка слота", error);
        std::printf("миграция: пробный слот свёртка вычищен\n");
    }
    std::printf("== измерение окончено; ЗАПОМНИТЕ: был ли хоть один вопрос "
                "системы. Повторный прогон подряд не должен задать ни одного. "
                "==\n\n");
    return 0;
}

#endif  // ZT_PROBE_DBUS

}  // namespace

int ztKeyringProbe(int argc, char** argv) {
    using zametti::BlobAad;
    using zametti::BlobKind;
    using zametti::Keyfile;
    using zametti::KeyringSecrets;
    using zametti::XChaChaCipher;

    QString storeId = QStringLiteral("01keyringprobe");
    bool keep = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--store-id") == 0 && i + 1 < argc)
            storeId = QString::fromUtf8(argv[++i]);
        else if (std::strcmp(argv[i], "--keep") == 0)
            keep = true;
    }

#ifdef Q_OS_MACOS
    // Сперва измерение сырыми SecItem* (см. шапку measureKeychain): его ответ
    // нужен ДО того, как доверять has()-по-атрибутам, и он не зависит от
    // готовности KeyringSecrets.
    const int measured = measureKeychain(keep);
    if (measured != 0) return measured;
#else
    (void)keep;
#endif
#ifdef ZT_PROBE_DBUS
    // Linux: состояние связки, замер «атрибуты у запертой», миграция
    // legacy → свёрток (см. шапку measureSecretService). На запертой связке
    // пробник честно останавливается — Unlock сам не зовёт.
    const int ssMeasured = measureSecretService(storeId);
    if (ssMeasured != 0) return ssMeasured;
    if (ssLockedState(nullptr) == 1) return 0;
#endif

    KeyringSecrets keyring;
    std::printf("служба секретов: %s\n",
                keyring.available() ? "доступна" : "НЕДОСТУПНА");
    if (!keyring.available()) return 1;

    QString error;

    // Ключ: родить, положить, достать, сверить через шифр, убрать.
    Keyfile made;
    if (!Keyfile::create(storeId, "пароль-пробника", {1, quint64(8) << 20},
                         &made, &error))
        return fail("create", error);
    QByteArray blob;
    if (!XChaChaCipher::make(made, &error)
             ->seal("проба", BlobAad{BlobKind::Journal, storeId, "probe.log"},
                    &blob, &error))
        return fail("seal", error);
    std::printf("ключ: рождён и запечатал пробный блоб\n");

    if (!keyring.storeKey(made, &error)) return fail("storeKey", error);
    std::printf("ключ: уложен в связку (метка zametti-key-%s)\n",
                storeId.toUtf8().constData());

    Keyfile loaded;
    if (!keyring.loadKey(storeId, &loaded, &error)) return fail("loadKey", error);
    QByteArray plain;
    if (!XChaChaCipher::make(loaded, &error)
             ->open(blob, BlobAad{BlobKind::Journal, storeId, "probe.log"},
                    &plain, &error) ||
        plain != "проба")
        return fail("ключ из связки не вскрыл печать исходного", error);
    std::printf("ключ: прочитан из связки, вскрывает печать исходного\n");

    if (!keyring.clearKey(storeId, &error)) return fail("clearKey", error);
    if (keyring.loadKey(storeId, &loaded, &error)) {
        std::printf("ПРОВАЛ: ключ читается после удаления\n");
        return 1;
    }
    std::printf("ключ: удалён, повторное чтение честно отказывает\n");

    // Пароль сервера: положить, достать, убрать.
    if (!keyring.setServerPassword(storeId, QStringLiteral("проба-пароля"), &error))
        return fail("setServerPassword", error);
    if (keyring.serverPassword(storeId, &error) != QStringLiteral("проба-пароля"))
        return fail("serverPassword вернул не то", error);
    std::printf("пароль сервера: уложен и прочитан тем же\n");
    if (!keyring.clearServerPassword(storeId, &error))
        return fail("clearServerPassword", error);
    if (!keyring.serverPassword(storeId, &error).isEmpty()) {
        std::printf("ПРОВАЛ: пароль читается после удаления\n");
        return 1;
    }
    std::printf("пароль сервера: удалён, повторное чтение честно отказывает\n");

    std::printf("keyring: приёмка пройдена\n");
    return 0;
}
