// KeyringSecrets на macOS: Keychain через Security.framework, все секреты —
// ОДНОЙ записью-свёртком (заказ владельца 30.08.2026; правила и формат —
// bundled_secrets.h, secret_bundle.h).
//
// Запись: kSecClassGenericPassword, service и метка io.zametti.zametti
// (слово владельца), account secrets. В данных — свёрток, в атрибуте
// kSecAttrGeneric — его индекс без секретов: «есть ли» читается одними
// атрибутами. kSecAttrSynchronizable не ставится — iCloud Keychain по
// умолчанию ВЫКЛ (обещание прежнего stub); галочка — отдельная работа.
//
// ПОЛИТИКА ВОПРОСОВ — требование владельца: максимум ОДИН системный вопрос
// после пересборки программы; «Always Allow» — навсегда, перезагрузка вопроса
// не возвращает; на новой машине — один раз. Что это значит здесь:
//
//   * запись НИКОГДА не пересоздаётся — writeBundle идёт через SecItemUpdate
//     и лишь при errSecItemNotFound через SecItemAdd: пересоздание сбрасывает
//     ACL записи и превращает «один вопрос» в «вопрос на каждом шагу»;
//   * свёрток читается раз за прогон (кэш BundledSecrets) — вопрос, если он
//     будет, случится единожды;
//   * has() ходит ТОЛЬКО по атрибутам (readIndex) — замер 30.08.2026 на этой
//     машине: ни SecItemAdd, ни чтение атрибутов, ни чтение данных СВОЕЙ
//     записи вопросов не поднимают; вопрос ждём лишь на чтении ДАННЫХ записи,
//     созданной другой сборкой (ad-hoc подпись даёт новый CDHash на каждую
//     пересборку — этот один вопрос без Developer ID неустраним и равен
//     разрешённому максимуму).
//
// Наборы этот файл не гоняют (живая связка — вещь машины): приёмка ручная,
// пробником `zametti-bench keyring`, и у пробника СВОЯ запись
// (io.zametti.zametti.probe) — боевую он не трогает.

#include "bundled_secrets.h"
#include "keyring_secrets.h"

#include <Security/Security.h>

namespace zametti {
namespace {

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

// База запроса: класс + service + account. Что вернуть или положить, каждая
// дверь добавляет сама.
CFMutableDictionaryRef baseQuery() {
    CFMutableDictionaryRef q = CFDictionaryCreateMutable(
        nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(q, kSecClass, kSecClassGenericPassword);
    CFDictionarySetValue(q, kSecAttrService, CFSTR("io.zametti.zametti"));
    CFDictionarySetValue(q, kSecAttrAccount, CFSTR("secrets"));
    return q;
}

CFDataRef cfData(const QByteArray& bytes) {
    return CFDataCreate(nullptr, (const UInt8*)bytes.constData(), bytes.size());
}

QByteArray fromCfData(CFDataRef data) {
    if (data == nullptr) return {};
    return QByteArray((const char*)CFDataGetBytePtr(data),
                      qsizetype(CFDataGetLength(data)));
}

// Двери BundledSecrets над Security.framework.
class KeychainBundle : public BundledSecrets {
public:
    bool available() const override { return true; }

protected:
    bool readBundle(QByteArray* data, bool* found, QString* error) override {
        *found = false;
        CFMutableDictionaryRef q = baseQuery();
        CFDictionarySetValue(q, kSecReturnData, kCFBooleanTrue);
        CFTypeRef out = nullptr;
        const OSStatus st = SecItemCopyMatching(q, &out);
        cfRelease(q);
        if (st == errSecItemNotFound) return true;   // записи ещё нет — не беда
        if (st != errSecSuccess) {
            if (error != nullptr)
                *error = QStringLiteral("keychain: %1").arg(osStatusText(st));
            return false;
        }
        *data = fromCfData((CFDataRef)out);
        cfRelease(out);
        *found = true;
        return true;
    }

    bool writeBundle(const QByteArray& bundle, const QByteArray& index,
                     QString* error) override {
        CFDataRef data = cfData(bundle);
        CFDataRef gen = cfData(index);

        // Сперва Update — запись не пересоздаётся (см. шапку). Add — только
        // для самой первой записи в жизни устройства.
        CFMutableDictionaryRef changes = CFDictionaryCreateMutable(
            nullptr, 0, &kCFTypeDictionaryKeyCallBacks,
            &kCFTypeDictionaryValueCallBacks);
        CFDictionarySetValue(changes, kSecValueData, data);
        CFDictionarySetValue(changes, kSecAttrGeneric, gen);
        CFMutableDictionaryRef q = baseQuery();
        OSStatus st = SecItemUpdate(q, changes);
        cfRelease(q);
        cfRelease(changes);

        if (st == errSecItemNotFound) {
            CFMutableDictionaryRef add = baseQuery();
            CFDictionarySetValue(add, kSecValueData, data);
            CFDictionarySetValue(add, kSecAttrGeneric, gen);
            CFDictionarySetValue(add, kSecAttrLabel, CFSTR("io.zametti.zametti"));
            st = SecItemAdd(add, nullptr);
            cfRelease(add);
        }
        cfRelease(gen);
        cfRelease(data);
        if (st != errSecSuccess) {
            if (error != nullptr)
                *error = QStringLiteral("keychain: %1").arg(osStatusText(st));
            return false;
        }
        return true;
    }

    QByteArray readIndex() override {
        // ТОЛЬКО атрибуты — без данных и без вопросов (замер в шапке).
        CFMutableDictionaryRef q = baseQuery();
        CFDictionarySetValue(q, kSecReturnAttributes, kCFBooleanTrue);
        CFTypeRef out = nullptr;
        const OSStatus st = SecItemCopyMatching(q, &out);
        cfRelease(q);
        if (st != errSecSuccess) return {};   // нет записи — нет и секретов
        const QByteArray index = fromCfData(
            (CFDataRef)CFDictionaryGetValue((CFDictionaryRef)out, kSecAttrGeneric));
        cfRelease(out);
        return index;
    }
};

}  // namespace

// KeyringSecrets — общий фасад трёх систем (заголовок один); на маке под ним
// живёт KeychainBundle.
struct KeyringSecrets::Impl {
    KeychainBundle bundle;
};

KeyringSecrets::KeyringSecrets() : impl_(std::make_shared<Impl>()) {}
KeyringSecrets::~KeyringSecrets() = default;

// Связка — часть macOS; отказ отдельной операции объясняется через error.
bool KeyringSecrets::available() const { return true; }

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
