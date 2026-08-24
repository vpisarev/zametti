// Keyfile: заворот и разворот мастер-ключа. Формат — см. шапку keyfile.h и
// раздел «Keyfile» в docs/zametti-storage.md.

#include "keyfile.h"

#include "times.h"

#include <sodium.h>

#include <QJsonDocument>
#include <QJsonValue>

namespace zametti {
namespace {

// Ключи JSON названы один раз.
constexpr char kVersionKey[] = "version";
constexpr char kStoreIdKey[] = "storeId";
constexpr char kCreatedKey[] = "created";
constexpr char kKdfKey[] = "kdf";
constexpr char kOpslimitKey[] = "opslimit";
constexpr char kMemlimitKey[] = "memlimit";
constexpr char kSaltKey[] = "salt";
constexpr char kCipherKey[] = "cipher";
constexpr char kNonceKey[] = "nonce";
constexpr char kKeyKey[] = "key";

// Имена алгоритмов в файле. Не украшение, а страж: keyfile, завёрнутый другим
// алгоритмом, честно не разворачивается, вместо того чтобы молча скормить
// байты не тому примитиву.
constexpr char kKdfName[] = "argon2id13";
constexpr char kCipherName[] = "xchacha20poly1305-ietf";

bool ensureSodium(QString* error) {
    if (sodium_init() >= 0) return true;
    if (error != nullptr) *error = QStringLiteral("libsodium failed to initialize");
    return false;
}

// AAD заворота: версия и storeId подписаны тегом — откат версии в тексте
// файла и подмена конверта между хранилищами валят разворот.
QByteArray keyfileAad(int version, const QString& storeId) {
    QByteArray aad("zametti-keyfile");
    aad.append('\0');
    aad.append(QByteArray::number(version));
    aad.append('\0');
    aad.append(storeId.toUtf8());
    return aad;
}

// Ключ из пароля. Копия пароля в utf8 стирается здесь же; сам QString стереть
// нельзя (implicit sharing), это честная граница гигиены — как и поле ввода.
bool deriveKek(const QString& password, const QByteArray& salt,
               const Keyfile::KdfParams& params, unsigned char* kek,
               size_t kekSize, QString* error) {
    QByteArray utf8 = password.toUtf8();
    const int rc = crypto_pwhash(
        kek, kekSize, utf8.constData(), size_t(utf8.size()),
        reinterpret_cast<const unsigned char*>(salt.constData()),
        params.opslimit, size_t(params.memlimitBytes),
        crypto_pwhash_ALG_ARGON2ID13);
    sodium_memzero(utf8.data(), size_t(utf8.size()));
    if (rc == 0) return true;
    if (error != nullptr)
        *error = QStringLiteral("Argon2id failed: not enough memory for %1 bytes")
                     .arg(params.memlimitBytes);
    return false;
}

}  // namespace

// Буфер живого ключа. Обнуляется в смерти ПОСЛЕДНЕЙ копии Keyfile — за это
// отвечает shared_ptr, а не дисциплина вызывающих.
struct Keyfile::LiveKey {
    QByteArray bytes;
    ~LiveKey() {
        if (!bytes.isEmpty()) sodium_memzero(bytes.data(), size_t(bytes.size()));
    }
};

Keyfile::~Keyfile() = default;

Keyfile::KdfParams Keyfile::defaults() {
    // Порог владельца, 24.08.2026: 512 МиБ × 4 прохода (~1.5 с на его машине).
    return KdfParams{4, quint64(512) << 20};
}

bool Keyfile::hasKey() const {
    return live_ != nullptr && live_->bytes.size() == kKeyBytes;
}

QByteArray Keyfile::key() const {
    Q_ASSERT(hasKey());
    return live_ != nullptr ? live_->bytes : QByteArray();
}

Keyfile Keyfile::fromLiveKey(const QString& storeId, const QByteArray& keyBytes) {
    Keyfile out;
    if (storeId.isEmpty() || keyBytes.size() != kKeyBytes) return out;
    out.storeId_ = storeId;
    out.live_ = std::make_shared<LiveKey>();
    out.live_->bytes = keyBytes;
    // Своя жизнь у буфера: копия, разделяющая байты с чужим QByteArray,
    // обнулила бы в смерти и его.
    out.live_->bytes.detach();
    return out;
}

bool Keyfile::create(const QString& storeId, const QString& password,
                     const KdfParams& params, Keyfile* out, QString* error) {
    Q_ASSERT(out != nullptr);
    Q_ASSERT(!storeId.isEmpty());
    if (!ensureSodium(error)) return false;
    if (storeId.isEmpty() || password.isEmpty() || params.opslimit == 0 ||
        params.memlimitBytes == 0) {
        if (error != nullptr) *error = QStringLiteral("keyfile: bad arguments");
        return false;
    }

    Keyfile made;
    made.storeId_ = storeId;
    made.created_ = store::isoNow();
    made.params_ = params;
    made.salt_.resize(crypto_pwhash_SALTBYTES);
    randombytes_buf(made.salt_.data(), size_t(made.salt_.size()));
    made.nonce_.resize(crypto_aead_xchacha20poly1305_ietf_NPUBBYTES);
    randombytes_buf(made.nonce_.data(), size_t(made.nonce_.size()));

    made.live_ = std::make_shared<LiveKey>();
    made.live_->bytes.resize(kKeyBytes);
    randombytes_buf(made.live_->bytes.data(), kKeyBytes);

    unsigned char kek[kKeyBytes];
    if (!deriveKek(password, made.salt_, params, kek, sizeof(kek), error))
        return false;

    const QByteArray aad = keyfileAad(made.version_, made.storeId_);
    made.wrapped_.resize(kKeyBytes + crypto_aead_xchacha20poly1305_ietf_ABYTES);
    unsigned long long wrappedLen = 0;
    const int rc = crypto_aead_xchacha20poly1305_ietf_encrypt(
        reinterpret_cast<unsigned char*>(made.wrapped_.data()), &wrappedLen,
        reinterpret_cast<const unsigned char*>(made.live_->bytes.constData()),
        kKeyBytes, reinterpret_cast<const unsigned char*>(aad.constData()),
        size_t(aad.size()), nullptr,
        reinterpret_cast<const unsigned char*>(made.nonce_.constData()), kek);
    sodium_memzero(kek, sizeof(kek));
    if (rc != 0 || wrappedLen != (unsigned long long)made.wrapped_.size()) {
        if (error != nullptr) *error = QStringLiteral("keyfile: seal failed");
        return false;
    }
    *out = made;
    return true;
}

bool Keyfile::parse(const QByteArray& bytes, QString* error) {
    QJsonParseError problem{};
    const QJsonDocument doc = QJsonDocument::fromJson(bytes, &problem);
    if (problem.error != QJsonParseError::NoError || !doc.isObject()) {
        if (error != nullptr)
            *error = QStringLiteral("keyfile is not a JSON object: %1")
                         .arg(problem.errorString());
        return false;
    }
    QJsonObject object = doc.object();

    const QJsonValue version = object.value(QLatin1String(kVersionKey));
    const int v = version.isDouble() ? version.toInt(kVersion) : kVersion;
    const QString id = object.value(QLatin1String(kStoreIdKey)).toString();
    if (id.isEmpty()) {
        if (error != nullptr) *error = QStringLiteral("keyfile: no storeId");
        return false;
    }

    // Версию новее нашей разбираем ровно настолько, чтобы её назвать: поля
    // могли поменять смысл, и будить такой ключ нельзя (страж в unwrap).
    if (v > kVersion) {
        version_ = v;
        storeId_ = id;
        return true;
    }

    const QString kdf = object.value(QLatin1String(kKdfKey)).toString();
    const QString cipher = object.value(QLatin1String(kCipherKey)).toString();
    if (kdf != QLatin1String(kKdfName) || cipher != QLatin1String(kCipherName)) {
        if (error != nullptr)
            *error = QStringLiteral("keyfile: unknown kdf/cipher: %1 / %2")
                         .arg(kdf, cipher);
        return false;
    }
    const quint64 ops = quint64(object.value(QLatin1String(kOpslimitKey)).toDouble());
    const quint64 mem = quint64(object.value(QLatin1String(kMemlimitKey)).toDouble());
    const QByteArray salt = QByteArray::fromBase64(
        object.value(QLatin1String(kSaltKey)).toString().toLatin1());
    const QByteArray nonce = QByteArray::fromBase64(
        object.value(QLatin1String(kNonceKey)).toString().toLatin1());
    const QByteArray wrapped = QByteArray::fromBase64(
        object.value(QLatin1String(kKeyKey)).toString().toLatin1());
    if (ops == 0 || mem == 0 || salt.size() != crypto_pwhash_SALTBYTES ||
        nonce.size() != crypto_aead_xchacha20poly1305_ietf_NPUBBYTES ||
        wrapped.size() != kKeyBytes + crypto_aead_xchacha20poly1305_ietf_ABYTES) {
        if (error != nullptr)
            *error = QStringLiteral("keyfile: malformed fields");
        return false;
    }

    version_ = v;
    storeId_ = id;
    created_ = object.value(QLatin1String(kCreatedKey)).toString();
    params_ = KdfParams{ops, mem};
    salt_ = salt;
    nonce_ = nonce;
    wrapped_ = wrapped;
    live_.reset();

    // Остальное — чужое знание: держим при себе и вернём в файл.
    for (const char* known : {kVersionKey, kStoreIdKey, kCreatedKey, kKdfKey,
                              kOpslimitKey, kMemlimitKey, kSaltKey, kCipherKey,
                              kNonceKey, kKeyKey})
        object.remove(QLatin1String(known));
    extra_ = object;
    return true;
}

QByteArray Keyfile::toBytes() const {
    // Keyfile из keyring конверта не имеет — собирать нечего.
    if (wrapped_.isEmpty()) return QByteArray();
    QJsonObject object = extra_;
    object.insert(QLatin1String(kVersionKey), version_);
    object.insert(QLatin1String(kStoreIdKey), storeId_);
    if (!created_.isEmpty()) object.insert(QLatin1String(kCreatedKey), created_);
    object.insert(QLatin1String(kKdfKey), QLatin1String(kKdfName));
    object.insert(QLatin1String(kOpslimitKey), double(params_.opslimit));
    object.insert(QLatin1String(kMemlimitKey), double(params_.memlimitBytes));
    object.insert(QLatin1String(kSaltKey), QString::fromLatin1(salt_.toBase64()));
    object.insert(QLatin1String(kCipherKey), QLatin1String(kCipherName));
    object.insert(QLatin1String(kNonceKey), QString::fromLatin1(nonce_.toBase64()));
    object.insert(QLatin1String(kKeyKey), QString::fromLatin1(wrapped_.toBase64()));
    return QJsonDocument(object).toJson(QJsonDocument::Indented);
}

bool Keyfile::unwrap(const QString& password, QString* error) {
    if (!ensureSodium(error)) return false;
    if (tooNew()) {
        if (error != nullptr)
            *error = QStringLiteral(
                "keyfile format %1 is newer than this build understands (%2) — "
                "update the program").arg(version_).arg(kVersion);
        return false;
    }
    if (wrapped_.isEmpty()) {
        if (error != nullptr) *error = QStringLiteral("keyfile: nothing to unwrap");
        return false;
    }

    unsigned char kek[kKeyBytes];
    if (!deriveKek(password, salt_, params_, kek, sizeof(kek), error))
        return false;

    auto fresh = std::make_shared<LiveKey>();
    fresh->bytes.resize(kKeyBytes);
    const QByteArray aad = keyfileAad(version_, storeId_);
    unsigned long long plainLen = 0;
    const int rc = crypto_aead_xchacha20poly1305_ietf_decrypt(
        reinterpret_cast<unsigned char*>(fresh->bytes.data()), &plainLen, nullptr,
        reinterpret_cast<const unsigned char*>(wrapped_.constData()),
        (unsigned long long)wrapped_.size(),
        reinterpret_cast<const unsigned char*>(aad.constData()),
        size_t(aad.size()),
        reinterpret_cast<const unsigned char*>(nonce_.constData()), kek);
    sodium_memzero(kek, sizeof(kek));
    if (rc != 0 || plainLen != kKeyBytes) {
        // Различить неверный пароль и порчу нельзя по построению AEAD; UI
        // отличает РОТАЦИЮ от них по другому признаку — keyfile развернулся,
        // а блобы не читаются (бриф, «правило когерентности»).
        if (error != nullptr)
            *error = QStringLiteral("wrong password, or the keyfile is corrupted");
        return false;
    }
    live_ = fresh;
    return true;
}

bool Keyfile::rewrap(const QString& oldPassword, const QString& newPassword,
                     const KdfParams& params, QString* error) {
    if (!hasKey() && !unwrap(oldPassword, error)) return false;
    if (newPassword.isEmpty() || params.opslimit == 0 || params.memlimitBytes == 0) {
        if (error != nullptr) *error = QStringLiteral("keyfile: bad arguments");
        return false;
    }

    // Новая соль и новый nonce: повторить nonce с другим ключом не страшно,
    // но правило «nonce не переиспользуется» дешевле держать без исключений.
    QByteArray salt(crypto_pwhash_SALTBYTES, 0);
    randombytes_buf(salt.data(), size_t(salt.size()));
    QByteArray nonce(crypto_aead_xchacha20poly1305_ietf_NPUBBYTES, 0);
    randombytes_buf(nonce.data(), size_t(nonce.size()));

    unsigned char kek[kKeyBytes];
    if (!deriveKek(newPassword, salt, params, kek, sizeof(kek), error))
        return false;

    const QByteArray aad = keyfileAad(version_, storeId_);
    QByteArray wrapped(kKeyBytes + crypto_aead_xchacha20poly1305_ietf_ABYTES, 0);
    unsigned long long wrappedLen = 0;
    const int rc = crypto_aead_xchacha20poly1305_ietf_encrypt(
        reinterpret_cast<unsigned char*>(wrapped.data()), &wrappedLen,
        reinterpret_cast<const unsigned char*>(live_->bytes.constData()),
        kKeyBytes, reinterpret_cast<const unsigned char*>(aad.constData()),
        size_t(aad.size()), nullptr,
        reinterpret_cast<const unsigned char*>(nonce.constData()), kek);
    sodium_memzero(kek, sizeof(kek));
    if (rc != 0 || wrappedLen != (unsigned long long)wrapped.size()) {
        if (error != nullptr) *error = QStringLiteral("keyfile: seal failed");
        return false;
    }
    params_ = params;
    salt_ = salt;
    nonce_ = nonce;
    wrapped_ = wrapped;
    return true;
}

}  // namespace zametti
