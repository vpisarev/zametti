// Обёртка блоба: формат — см. шапку blob_cipher.h и раздел «The cloud» в
// docs/info/zametti-storage.md.

#include "blob_cipher.h"

#include <sodium.h>

#include <cstring>

namespace zametti {
namespace {

constexpr char kMagic[4] = {'Z', 'B', 'L', 'B'};
constexpr int kNonceBytes = 24;   // crypto_aead_xchacha20poly1305_ietf_NPUBBYTES
constexpr int kTagBytes = 16;     // crypto_aead_xchacha20poly1305_ietf_ABYTES
constexpr int kHeaderBytes = int(sizeof(kMagic)) + 1 + kNonceBytes;

static_assert(kNonceBytes == crypto_aead_xchacha20poly1305_ietf_NPUBBYTES);
static_assert(kTagBytes == crypto_aead_xchacha20poly1305_ietf_ABYTES);
static_assert(XChaChaCipher::kOverhead == kHeaderBytes + kTagBytes);

const char* kindName(BlobKind kind) {
    switch (kind) {
        case BlobKind::Journal: return "journal";
        case BlobKind::Attachment: return "attachment";
    }
    Q_ASSERT(false);
    return "?";
}

// Канонические байты AAD. Версию подаёт шифр: печать — свою, вскрытие — из
// заголовка блоба, так что сброшенная в заголовке версия меняет AAD и валит
// тег (downgrade не тихий).
QByteArray aadBytes(int version, const BlobAad& aad) {
    QByteArray out("zametti-blob");
    out.append('\0');
    out.append(QByteArray::number(version));
    out.append('\0');
    out.append(kindName(aad.kind));
    out.append('\0');
    out.append(aad.storeId.toUtf8());
    out.append('\0');
    out.append(aad.name.toUtf8());
    return out;
}

}  // namespace

std::shared_ptr<BlobCipher> XChaChaCipher::make(const Keyfile& keyfile,
                                                QString* error) {
    if (sodium_init() < 0) {
        if (error != nullptr) *error = QStringLiteral("libsodium failed to initialize");
        return nullptr;
    }
    if (!keyfile.hasKey()) {
        if (error != nullptr)
            *error = QStringLiteral("the keyfile carries no live key — unwrap it first");
        return nullptr;
    }
    // make_shared сюда не достаёт: конструктор protected нарочно.
    return std::shared_ptr<BlobCipher>(new XChaChaCipher(keyfile));
}

XChaChaCipher::XChaChaCipher(const Keyfile& keyfile) : keyfile_(keyfile) {}

bool XChaChaCipher::seal(const QByteArray& plain, const BlobAad& aad,
                         QByteArray* blob, QString* error) const {
    Q_ASSERT(blob != nullptr);
    Q_ASSERT(keyfile_.hasKey());
    const QByteArray key = keyfile_.key();
    const QByteArray bound = aadBytes(kWrapVersion, aad);

    QByteArray out(kHeaderBytes + plain.size() + kTagBytes, 0);
    std::memcpy(out.data(), kMagic, sizeof(kMagic));
    out[sizeof(kMagic)] = char(kWrapVersion);
    unsigned char* nonce =
        reinterpret_cast<unsigned char*>(out.data() + sizeof(kMagic) + 1);
    randombytes_buf(nonce, kNonceBytes);

    unsigned long long sealedLen = 0;
    const int rc = crypto_aead_xchacha20poly1305_ietf_encrypt(
        reinterpret_cast<unsigned char*>(out.data() + kHeaderBytes), &sealedLen,
        reinterpret_cast<const unsigned char*>(plain.constData()),
        (unsigned long long)plain.size(),
        reinterpret_cast<const unsigned char*>(bound.constData()),
        size_t(bound.size()), nullptr, nonce,
        reinterpret_cast<const unsigned char*>(key.constData()));
    if (rc != 0 || sealedLen != (unsigned long long)(plain.size() + kTagBytes)) {
        if (error != nullptr) *error = QStringLiteral("blob seal failed");
        return false;
    }
    *blob = out;
    return true;
}

bool XChaChaCipher::open(const QByteArray& blob, const BlobAad& aad,
                         QByteArray* plain, QString* error) const {
    Q_ASSERT(plain != nullptr);
    Q_ASSERT(keyfile_.hasKey());
    if (blob.size() < kHeaderBytes + kTagBytes ||
        std::memcmp(blob.constData(), kMagic, sizeof(kMagic)) != 0) {
        if (error != nullptr)
            *error = QStringLiteral("%1: not a zametti blob").arg(aad.name);
        return false;
    }
    const int version = (unsigned char)blob[sizeof(kMagic)];
    if (version > kWrapVersion || version == 0) {
        if (error != nullptr)
            *error = QStringLiteral(
                "%1: blob wrap v%2 is newer than this build understands (v%3) — "
                "update the program").arg(aad.name).arg(version).arg(kWrapVersion);
        return false;
    }

    const QByteArray key = keyfile_.key();
    // Версия — ИЗ ЗАГОЛОВКА: печатавший подписал свою, и подкрутка байта
    // заголовка честно валит тег ниже.
    const QByteArray bound = aadBytes(version, aad);
    QByteArray out(blob.size() - kHeaderBytes - kTagBytes, 0);
    unsigned long long plainLen = 0;
    const int rc = crypto_aead_xchacha20poly1305_ietf_decrypt(
        reinterpret_cast<unsigned char*>(out.data()), &plainLen, nullptr,
        reinterpret_cast<const unsigned char*>(blob.constData() + kHeaderBytes),
        (unsigned long long)(blob.size() - kHeaderBytes),
        reinterpret_cast<const unsigned char*>(bound.constData()),
        size_t(bound.size()),
        reinterpret_cast<const unsigned char*>(blob.constData() + sizeof(kMagic) + 1),
        reinterpret_cast<const unsigned char*>(key.constData()));
    if (rc != 0 || plainLen != (unsigned long long)out.size()) {
        // Порча, подмена (имя/тип/хранилище), чужой ключ — по построению AEAD
        // неразличимы, и различать их не наше дело: блоб отвергнут целиком.
        if (error != nullptr)
            *error = QStringLiteral("%1: blob rejected (corrupted, mixed up, or "
                                    "sealed with another key)").arg(aad.name);
        return false;
    }
    *plain = out;
    return true;
}

}  // namespace zametti
