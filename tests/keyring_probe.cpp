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
        const bool same = QByteArray((const char*)CFDataGetBytePtr((CFDataRef)out),
                                     int(CFDataGetLength((CFDataRef)out))) == blob;
        cfRelease(out);
        if (!same)
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
