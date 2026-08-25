// KeyringSecrets под Windows — Credential Manager (m17; Windows-заход,
// 26.08.2026, решение владельца: спрашивать пароль каждый запуск неудобно).
//
// Служба часть системы, отдельной библиотеки не тянет: CredWriteW/CredReadW/
// CredDeleteW живут в Advapi32, которая в программе и так есть. Секрет лежит
// зашифрованным ключом ПОЛЬЗОВАТЕЛЯ (DPAPI изнутри службы) и доступен только
// его сеансу — та же гарантия, что даёт Secret Service под Linux.
//
// ИМЕНА ТЕ ЖЕ, ЧТО У SECRET SERVICE, и это нарочно: одно хранилище, открытое
// на двух системах, показывает в связке одинаково названные элементы, и
// человеку не надо держать в голове две схемы. У Secret Service имя — метка
// плюс атрибуты {application, storeId, what}; у Windows атрибутов нет вовсе,
// поиск идёт по TargetName. Поэтому вся тройка сворачивается в имя:
//
//   zametti-key-<storeId>       мастер-ключ, 32 байта как есть
//   zametti-webdav-<storeId>    пароль сервера, UTF-8
//
// Слово «zametti» в начале — это и есть бывший атрибут application: чужого с
// таким префиксом в связке не будет.

#include "keyring_secrets.h"

#include <QString>

#include <windows.h>
#include <wincred.h>

namespace zametti {
namespace {

// Потолок Windows на секрет — 512 * 5 = 2560 байт (CRED_MAX_CREDENTIAL_BLOB_SIZE).
// Ключ у нас 32 байта, пароль сервера — человеческий: обоим до потолка далеко.
// Проверка всё равно стоит, и молчать при переполнении нельзя: молча
// невыполненное сохранение пароля выглядит как «keyring не работает».
constexpr DWORD kMaxBlob = CRED_MAX_CREDENTIAL_BLOB_SIZE;

QString targetFor(const QString& storeId, const char* what) {
    return QStringLiteral("zametti-%1-%2").arg(QLatin1String(what), storeId);
}

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

bool fail(QString* error, const QString& what, DWORD code) {
    if (error != nullptr)
        *error = QStringLiteral("%1: %2").arg(what, lastErrorText(code));
    return false;
}

// Записать секрет под именем. Перезапись — штатное поведение CredWriteW, так
// же как у putSecret через Secret Service: второй разворот на той же машине
// просто кладёт ключ поверх.
bool putSecret(const QString& target, const QByteArray& secret, QString* error) {
    if (secret.size() > qsizetype(kMaxBlob)) {
        if (error != nullptr)
            *error = QStringLiteral("secret for %1 is %2 bytes, the Windows limit is %3")
                         .arg(target).arg(secret.size()).arg(kMaxBlob);
        return false;
    }
    std::wstring wtarget(size_t(target.size()), L'\0');
    target.toWCharArray(wtarget.data());
    // UserName у обобщённого секрета ни на что не влияет, но пустым его
    // оставлять не стоит: в окне «Диспетчер учётных данных» человек видит
    // именно эту колонку.
    std::wstring user = L"zametti";

    CREDENTIALW cred{};
    cred.Type = CRED_TYPE_GENERIC;
    cred.TargetName = wtarget.data();
    cred.UserName = user.data();
    cred.CredentialBlobSize = DWORD(secret.size());
    cred.CredentialBlob =
        reinterpret_cast<LPBYTE>(const_cast<char*>(secret.constData()));
    // LOCAL_MACHINE, а не SESSION: секрет обязан пережить перезагрузку, иначе
    // пароль спрашивался бы каждое утро — ровно то, от чего мы уходим.
    cred.Persist = CRED_PERSIST_LOCAL_MACHINE;

    if (CredWriteW(&cred, 0) != FALSE) return true;
    return fail(error, QStringLiteral("cannot store %1 in the credential manager").arg(target),
                GetLastError());
}

// Прочитать секрет. found отличает «нет такого» от «беда»: первое — обычное
// дело (ключа ещё не клали), второе — повод сказать вслух.
QByteArray getSecret(const QString& target, bool* found, QString* error) {
    *found = false;
    std::wstring wtarget(size_t(target.size()), L'\0');
    target.toWCharArray(wtarget.data());

    PCREDENTIALW cred = nullptr;
    if (CredReadW(wtarget.c_str(), CRED_TYPE_GENERIC, 0, &cred) == FALSE) {
        const DWORD code = GetLastError();
        if (code != ERROR_NOT_FOUND)
            fail(error, QStringLiteral("cannot read %1 from the credential manager").arg(target),
                 code);
        return {};
    }
    QByteArray out(reinterpret_cast<const char*>(cred->CredentialBlob),
                   qsizetype(cred->CredentialBlobSize));
    // Копию системы затираем ДО CredFree: она лежит в куче процесса, и
    // освобождённая память ключ бы ещё помнила.
    if (cred->CredentialBlob != nullptr && cred->CredentialBlobSize > 0)
        SecureZeroMemory(cred->CredentialBlob, cred->CredentialBlobSize);
    CredFree(cred);
    *found = true;
    return out;
}

// Удалить. «Не было» — это удача, а не беда: clearKey зовут и там, где ключа
// могло не быть вовсе (DBus-версия ведёт себя так же).
bool removeSecret(const QString& target, QString* error) {
    std::wstring wtarget(size_t(target.size()), L'\0');
    target.toWCharArray(wtarget.data());
    if (CredDeleteW(wtarget.c_str(), CRED_TYPE_GENERIC, 0) != FALSE) return true;
    const DWORD code = GetLastError();
    if (code == ERROR_NOT_FOUND) return true;
    return fail(error, QStringLiteral("cannot delete %1 from the credential manager").arg(target),
                code);
}

}  // namespace

// Состояния у нас нет: каждый вызов ходит в систему сам. Impl оставлен пустым
// ради общего заголовка — он один на все три воплощения.
struct KeyringSecrets::Impl {};

KeyringSecrets::KeyringSecrets() : impl_(std::make_shared<Impl>()) {}
KeyringSecrets::~KeyringSecrets() = default;

// Служба учётных данных — часть Windows; шины, которой может не быть, здесь
// нет. Отказ отдельной операции объясняется через error, как и у DBus-версии.
bool KeyringSecrets::available() const { return true; }

bool KeyringSecrets::loadKey(const QString& storeId, Keyfile* out, QString* error) {
    Q_ASSERT(out != nullptr);
    bool found = false;
    const QByteArray bytes = getSecret(targetFor(storeId, "key"), &found, error);
    if (!found) {
        if (error != nullptr && error->isEmpty())
            *error = QStringLiteral("no key for %1 in the credential manager").arg(storeId);
        return false;
    }
    const Keyfile made = Keyfile::fromLiveKey(storeId, bytes);
    if (!made.hasKey()) {
        if (error != nullptr)
            *error = QStringLiteral("credential manager item for %1 is not a %2-byte key")
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
    return putSecret(targetFor(keyfile.storeId(), "key"), keyfile.key(), error);
}

bool KeyringSecrets::clearKey(const QString& storeId, QString* error) {
    return removeSecret(targetFor(storeId, "key"), error);
}

QString KeyringSecrets::serverPassword(const QString& storeId, QString* error) {
    bool found = false;
    const QByteArray bytes = getSecret(targetFor(storeId, "webdav"), &found, error);
    if (!found) {
        if (error != nullptr && error->isEmpty())
            *error = QStringLiteral("no server password for %1 in the credential manager")
                         .arg(storeId);
        return QString();
    }
    return QString::fromUtf8(bytes);
}

bool KeyringSecrets::setServerPassword(const QString& storeId,
                                       const QString& password, QString* error) {
    return putSecret(targetFor(storeId, "webdav"), password.toUtf8(), error);
}

bool KeyringSecrets::clearServerPassword(const QString& storeId, QString* error) {
    return removeSecret(targetFor(storeId, "webdav"), error);
}

}  // namespace zametti
