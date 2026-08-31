// KeyringSecrets под Windows — Credential Manager, все секреты — ОДНОЙ
// записью-свёртком (заказ владельца 30.08.2026; правила и формат —
// bundled_secrets.h, secret_bundle.h; мак и Linux переехали первыми, этот
// файл — третьим, 31.08.2026).
//
// Служба часть системы, отдельной библиотеки не тянет: CredWriteW/CredReadW/
// CredDeleteW живут в Advapi32, которая в программе и так есть. Секрет лежит
// зашифрованным ключом ПОЛЬЗОВАТЕЛЯ (DPAPI изнутри службы) и доступен только
// его сеансу — та же гарантия, что даёт Secret Service под Linux.
//
// Запись: TargetName «zametti-secrets», CRED_TYPE_GENERIC, LOCAL_MACHINE.
// В блобе — свёрток; ИНДЕКС — в CredentialAttributes кусками index0..indexN
// (значение атрибута ≤ 256 байт, атрибутов до 64 — упереться некуда).
// Замечание честности: «только атрибуты» Windows читать не умеет — CredReadW
// отдаёт и блоб; это допустимо, потому что довод «индекс без данных» — про
// СИСТЕМНЫЕ ВОПРОСЫ, которых у Credential Manager не бывает (DPAPI прозрачен);
// копия блоба затирается SecureZeroMemory до CredFree, как и раньше.
//
// «Не пересоздавать запись» здесь даром: CredWriteW перезаписывает на месте,
// ACL-беды мака у Windows нет.
//
// МИГРАЦИЯ: прежде этот файл держал запись-на-секрет
// (zametti-{key,webdav,password}-<storeId>). Первое чтение свёртка — данных
// ИЛИ индекса (вопросов Windows не задаёт, потому индексное чтение тоже
// вправе мигрировать), — не найдя его, собирает старые записи в свёрток и,
// только записав его, удаляет их (решение владельца 31.08): оставленные, они
// лгали бы «Диспетчеру учётных данных» и воскресали бы при откате версии.

#include "bundled_secrets.h"
#include "keyring_secrets.h"

#include <QString>

#include <windows.h>
#include <wincred.h>

#include <string>
#include <vector>

namespace zametti {
namespace {

// Потолок Windows на блоб — 5 * 512 = 2560 байт: так CRED_MAX_CREDENTIAL_BLOB_SIZE
// определён в современном Windows SDK, и таков реальный лимит ОС (Vista+).
// Заголовок mingw несёт устаревшее значение 512, поэтому константа своя, а не
// из заголовка. Свёрток стоит ~120–140 байт на хранилище (ключ 32 байта,
// человеческие пароли, тонкая CBOR-обвязка) — в 2560 влезает больше десятка
// хранилищ; переполнение всё равно говорит вслух: молча невыполненное
// сохранение выглядит как «keyring не работает». (План Б на далёкое будущее —
// резать свёрток на zametti-secrets-<n> — записан в отчёте, не в коде.)
constexpr DWORD kMaxBlob = 5 * 512;

// Значение одного CredentialAttribute — не длиннее 256 байт (CRED_MAX_VALUE_SIZE).
constexpr DWORD kMaxAttrValue = 256;
constexpr DWORD kMaxAttrCount = 64;

const wchar_t kBundleTarget[] = L"zametti-secrets";

// Сообщение системы по коду ошибки. Без него в error уезжало бы голое число, а
// разбирать «GetLastError=1168» человеку не по чину.
QString lastErrorText(DWORD code) {
    LPWSTR text = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, reinterpret_cast<LPWSTR>(&text), 0, nullptr);
    QString out = length > 0 && text != nullptr
                      ? QString::fromWCharArray(text, int(length)).trimmed()
                      : QStringLiteral("error %1").arg(code);
    if (text != nullptr) LocalFree(text);
    return out;
}

bool failWith(QString* error, const QString& what, DWORD code) {
    if (error != nullptr)
        *error = QStringLiteral("%1: %2").arg(what, lastErrorText(code));
    return false;
}

// Затереть и вернуть копию блоба записи: система держит его в куче процесса,
// и освобождённая память секрет бы ещё помнила.
QByteArray takeBlob(PCREDENTIALW cred) {
    QByteArray out(reinterpret_cast<const char*>(cred->CredentialBlob),
                   qsizetype(cred->CredentialBlobSize));
    if (cred->CredentialBlob != nullptr && cred->CredentialBlobSize > 0)
        SecureZeroMemory(cred->CredentialBlob, cred->CredentialBlobSize);
    return out;
}

// Двери BundledSecrets над Credential Manager.
class WinCredBundle : public BundledSecrets {
public:
    bool available() const override { return true; }

protected:
    bool readBundle(QByteArray* data, bool* found, QString* error) override {
        *found = false;
        PCREDENTIALW cred = nullptr;
        if (CredReadW(kBundleTarget, CRED_TYPE_GENERIC, 0, &cred) == FALSE) {
            const DWORD code = GetLastError();
            if (code != ERROR_NOT_FOUND)
                return failWith(error,
                                QStringLiteral("cannot read the secrets bundle"),
                                code);
            // Свёртка нет; единственный случай, когда это не «пустая связка»,
            // — записи прежнего формата: собрать и переехать.
            return migrateLegacy(data, found, error);
        }
        *data = takeBlob(cred);
        CredFree(cred);
        *found = true;
        return true;
    }

    bool writeBundle(const QByteArray& bundle, const QByteArray& index,
                     QString* error) override {
        if (bundle.size() > qsizetype(kMaxBlob)) {
            if (error != nullptr)
                *error = QStringLiteral(
                             "the secrets bundle is %1 bytes, the Windows limit is %2")
                             .arg(bundle.size())
                             .arg(kMaxBlob);
            return false;
        }
        // Индекс — кусками по атрибутам index0..indexN.
        const DWORD chunks =
            DWORD((index.size() + qsizetype(kMaxAttrValue) - 1) /
                  qsizetype(kMaxAttrValue));
        if (chunks > kMaxAttrCount) {
            if (error != nullptr)
                *error = QStringLiteral("the bundle index does not fit into %1 "
                                        "credential attributes")
                             .arg(kMaxAttrCount);
            return false;
        }
        std::vector<CREDENTIAL_ATTRIBUTEW> attrs(static_cast<size_t>(chunks));
        std::vector<std::wstring> names(static_cast<size_t>(chunks));
        for (DWORD i = 0; i < chunks; ++i) {
            const qsizetype at = qsizetype(i) * qsizetype(kMaxAttrValue);
            const qsizetype len =
                qMin(qsizetype(kMaxAttrValue), index.size() - at);
            names[i] = L"index" + std::to_wstring(i);
            attrs[i] = {};
            attrs[i].Keyword = names[i].data();
            attrs[i].ValueSize = DWORD(len);
            attrs[i].Value = reinterpret_cast<LPBYTE>(
                const_cast<char*>(index.constData() + at));
        }

        std::wstring target = kBundleTarget;
        // UserName ни на что не влияет, но пустым его оставлять не стоит: в
        // окне «Диспетчер учётных данных» человек видит именно эту колонку.
        std::wstring user = L"zametti";
        CREDENTIALW cred{};
        cred.Type = CRED_TYPE_GENERIC;
        cred.TargetName = target.data();
        cred.UserName = user.data();
        cred.CredentialBlobSize = DWORD(bundle.size());
        cred.CredentialBlob =
            reinterpret_cast<LPBYTE>(const_cast<char*>(bundle.constData()));
        cred.AttributeCount = chunks;
        cred.Attributes = attrs.empty() ? nullptr : attrs.data();
        // LOCAL_MACHINE, а не SESSION: секрет обязан пережить перезагрузку,
        // иначе пароль спрашивался бы каждое утро — то, от чего мы уходим.
        cred.Persist = CRED_PERSIST_LOCAL_MACHINE;

        if (CredWriteW(&cred, 0) != FALSE) return true;
        return failWith(error, QStringLiteral("cannot store the secrets bundle"),
                        GetLastError());
    }

    QByteArray readIndex() override {
        PCREDENTIALW cred = nullptr;
        if (CredReadW(kBundleTarget, CRED_TYPE_GENERIC, 0, &cred) == FALSE) {
            if (GetLastError() != ERROR_NOT_FOUND) return {};
            // Вопросов Windows не задаёт — индексное чтение вправе мигрировать
            // само (см. шапку): иначе до первого чтения ДАННЫХ has() не видел
            // бы записей прежнего формата и окно занижало бы факты.
            QByteArray bytes;
            bool found = false;
            if (!migrateLegacy(&bytes, &found, nullptr) || !found) return {};
            SecretBundle moved;
            if (!moved.parse(bytes)) return {};
            return moved.index();
        }
        QByteArray index;
        for (DWORD i = 0; i < cred->AttributeCount; ++i) {
            const std::wstring want = L"index" + std::to_wstring(i);
            for (DWORD k = 0; k < cred->AttributeCount; ++k) {
                const CREDENTIAL_ATTRIBUTEW& a = cred->Attributes[k];
                if (a.Keyword == nullptr || want != a.Keyword) continue;
                index.append(reinterpret_cast<const char*>(a.Value),
                             qsizetype(a.ValueSize));
                break;
            }
        }
        const QByteArray blob = takeBlob(cred);
        CredFree(cred);
        if (!index.isEmpty()) return index;
        // Атрибутов нет (wine их не хранит вовсе — замер 31.08 пробником) —
        // индекс достаётся из самого свёртка: на Windows чтение блоба вопросов
        // не поднимает, доктрина «индекс без данных» здесь про вопросы, а не
        // про байты.
        SecretBundle parsed;
        if (!parsed.parse(blob)) return {};
        return parsed.index();
    }

private:
    bool migrateLegacy(QByteArray* data, bool* found, QString* error) {
        DWORD count = 0;
        PCREDENTIALW* creds = nullptr;
        if (CredEnumerateW(L"zametti-*", 0, &count, &creds) == FALSE)
            return true;   // нечего перечислить — пусть свёртка просто нет
        SecretBundle moved;
        std::vector<std::wstring> emptied;
        for (DWORD i = 0; i < count; ++i) {
            const QString target = QString::fromWCharArray(creds[i]->TargetName);
            const char* what = nullptr;
            SecretStore::Secret slot = SecretStore::Secret::Key;
            if (target.startsWith(QLatin1String("zametti-key-"))) {
                what = "zametti-key-";
                slot = SecretStore::Secret::Key;
            } else if (target.startsWith(QLatin1String("zametti-webdav-"))) {
                what = "zametti-webdav-";
                slot = SecretStore::Secret::ServerPassword;
            } else if (target.startsWith(QLatin1String("zametti-password-"))) {
                what = "zametti-password-";
                slot = SecretStore::Secret::EncryptionPassword;
            } else {
                continue;   // zametti-secrets и чужое — не трогаем
            }
            const QString storeId = target.mid(qsizetype(strlen(what)));
            if (storeId.isEmpty()) continue;
            moved.put(storeId, slot, takeBlob(creds[i]));
            emptied.push_back(std::wstring(
                reinterpret_cast<const wchar_t*>(creds[i]->TargetName)));
        }
        CredFree(creds);
        if (moved.isEmpty()) return true;   // чистая связка — не миграция

        const QByteArray bytes = moved.toBytes();
        if (!writeBundle(bytes, moved.index(), error)) return false;
        // Только после удачной записи: старые записи, оставленные жить, лгали
        // бы витрине и воскресали бы при откате версии программы.
        for (const std::wstring& target : emptied)
            CredDeleteW(target.c_str(), CRED_TYPE_GENERIC, 0);
        *data = bytes;
        *found = true;
        return true;
    }
};

}  // namespace

// KeyringSecrets — общий фасад трёх систем (заголовок один); под Windows под
// ним живёт WinCredBundle.
struct KeyringSecrets::Impl {
    WinCredBundle bundle;
};

KeyringSecrets::KeyringSecrets() : impl_(std::make_shared<Impl>()) {}
KeyringSecrets::~KeyringSecrets() = default;

// Служба учётных данных — часть Windows; шины, которой может не быть, здесь
// нет. Отказ отдельной операции объясняется через error.
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
