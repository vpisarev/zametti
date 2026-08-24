// Тест-векторы крипто-примитивов облачной синхронизации (m17, сессия 3).
//
// Проверяется НАШ ВЫЗОВ libsodium, а не сама библиотека: перепутанный порядок
// аргументов, соль на месте пароля или не тот вариант алгоритма дают «работающее»
// шифрование, которое не прочитает ни одно другое устройство. Поэтому векторы —
// ДО первого класса, как велит бриф.
//
// XChaCha20-Poly1305 — байты из draft-irtf-cfrg-xchacha-03 дословно:
// §2.2.1 (HChaCha20) и приложение A.3 (AEAD, 114 байт про sunscreen).
//
// Argon2id — НЕ дословный вектор RFC 9106, и вот почему: канонический вектор
// RFC считан с parallelism=4, secret и ad, а crypto_pwhash в libsodium
// сознательно сужен — только p=1, соль ровно 16 байт, без secret/ad, то есть
// вектор из текста RFC через него прогнать физически нечем. Константы ниже
// посчитаны 24.08.2026 ДВУМЯ независимыми чужими судьями и сошлись побайтово:
//   * argon2-cffi 25.1.0 — биндинг эталонной phc-winner-argon2 (реализация
//     авторов алгоритма, по ней написан RFC);
//   * argon2pure 1.3 — независимая чистопитоновая реализация.
// (uv run --with argon2-cffi / --with argon2pure; hash_secret_raw(...,
// parallelism=1, type=ID, version=19) и argon2(..., ARGON2ID, 0x13).)

#include <sodium.h>

#include <QByteArray>

#include <cstring>

#include "test_util.h"

namespace {

QByteArray hex(const char* s) { return QByteArray::fromHex(s); }

// --- HChaCha20, §2.2.1 драфта ---

void checkHChaCha20() {
    const QByteArray key = hex(
        "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f");
    const QByteArray nonce = hex("000000090000004a0000000031415927");
    const QByteArray subkey = hex(
        "82413b4227b27bfed30e42508a877d73a0f9e4d58a74a853c12ec41326d3ecdc");

    unsigned char out[crypto_core_hchacha20_OUTPUTBYTES];
    const int rc = crypto_core_hchacha20(
        out, reinterpret_cast<const unsigned char*>(nonce.constData()),
        reinterpret_cast<const unsigned char*>(key.constData()), nullptr);
    ZT_TRUE("HChaCha20 отработал", rc == 0);
    ZT_TRUE("подключ HChaCha20 совпал с §2.2.1",
            QByteArray(reinterpret_cast<char*>(out), sizeof(out)) == subkey);
}

// --- AEAD XChaCha20-Poly1305, приложение A.3 драфта ---

void checkAead() {
    const QByteArray plain(
        "Ladies and Gentlemen of the class of '99: If I could offer you "
        "only one tip for the future, sunscreen would be it.");
    ZT_TRUE("длина открытого текста из драфта — 114", plain.size() == 114);
    const QByteArray aad = hex("50515253c0c1c2c3c4c5c6c7");
    const QByteArray key = hex(
        "808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f");
    const QByteArray nonce = hex("404142434445464748494a4b4c4d4e4f5051525354555657");
    const QByteArray wantCipher = hex(
        "bd6d179d3e83d43b9576579493c0e939572a1700252bfaccbed2902c21396cbb"
        "731c7f1b0b4aa6440bf3a82f4eda7e39ae64c6708c54c216cb96b72e1213b452"
        "2f8c9ba40db5d945b11b69b982c1bb9e3f3fac2bc369488f76b2383565d3fff9"
        "21f9664c97637da9768812f615c68b13b52e");
    const QByteArray wantTag = hex("c0875924c1c7987947deafd8780acf49");

    auto u = [](const QByteArray& b) {
        return reinterpret_cast<const unsigned char*>(b.constData());
    };

    QByteArray cipher(plain.size(), 0);
    unsigned char tag[crypto_aead_xchacha20poly1305_ietf_ABYTES];
    unsigned long long tagLen = 0;
    int rc = crypto_aead_xchacha20poly1305_ietf_encrypt_detached(
        reinterpret_cast<unsigned char*>(cipher.data()), tag, &tagLen,
        u(plain), plain.size(), u(aad), aad.size(), nullptr, u(nonce), u(key));
    ZT_TRUE("шифрование отработало", rc == 0);
    ZT_TRUE("шифротекст совпал с A.3", cipher == wantCipher);
    ZT_TRUE("длина тега — как у libsodium", tagLen == sizeof(tag));
    ZT_TRUE("тег совпал с A.3",
            QByteArray(reinterpret_cast<char*>(tag), int(tagLen)) == wantTag);

    // Обратный ход: расшифровка возвращает исходник.
    QByteArray back(cipher.size(), 0);
    rc = crypto_aead_xchacha20poly1305_ietf_decrypt_detached(
        reinterpret_cast<unsigned char*>(back.data()), nullptr,
        u(cipher), cipher.size(), u(wantTag), u(aad), aad.size(), u(nonce), u(key));
    ZT_TRUE("расшифровка отработала", rc == 0);
    ZT_TRUE("расшифрованное равно исходнику", back == plain);

    // Порча любого участника — отказ, а не «чуть-чуть другой» текст.
    auto rejects = [&](const QByteArray& c, const QByteArray& t,
                       const QByteArray& a) {
        QByteArray sink(c.size(), 0);
        return crypto_aead_xchacha20poly1305_ietf_decrypt_detached(
                   reinterpret_cast<unsigned char*>(sink.data()), nullptr,
                   u(c), c.size(), u(t), u(a), a.size(), u(nonce), u(key)) != 0;
    };
    QByteArray spoiled = cipher;
    spoiled[7] = char(spoiled[7] ^ 0x01);
    ZT_TRUE("бит в шифротексте — отказ", rejects(spoiled, wantTag, aad));
    spoiled = wantTag;
    spoiled[0] = char(spoiled[0] ^ 0x80);
    ZT_TRUE("бит в теге — отказ", rejects(cipher, spoiled, aad));
    spoiled = aad;
    spoiled[3] = char(spoiled[3] ^ 0x01);
    ZT_TRUE("бит в AAD — отказ", rejects(cipher, wantTag, spoiled));
}

// --- Argon2id: KAT эталонной реализации (см. шапку файла) ---

void checkArgon2id() {
    struct Kat {
        const char* password;
        const char* salt;        // ровно 16 байт — требование crypto_pwhash
        unsigned long long opslimit;
        size_t memlimitBytes;
        const char* wantHex;
    };
    const Kat kats[] = {
        {"correct horse battery staple", "zametti-kat-salt", 3, 64u << 20,
         "5549fe19e758d0847c5b8432ec1da8f516ee3c73f672e87df76a996f86462cea"},
        {"zametti", "0123456789abcdef", 2, 64u << 10,
         "a142764927fefd7740f2a156ae3cc5fde5bf0ff54f396657ed6dad6c640b3cd4"},
    };
    for (const Kat& kat : kats) {
        const QByteArray salt(kat.salt);
        ZT_TRUE("соль ровно 16 байт", salt.size() == crypto_pwhash_SALTBYTES);
        unsigned char out[32];
        const int rc = crypto_pwhash(
            out, sizeof(out), kat.password, std::strlen(kat.password),
            reinterpret_cast<const unsigned char*>(salt.constData()),
            kat.opslimit, kat.memlimitBytes, crypto_pwhash_ALG_ARGON2ID13);
        ZT_TRUE("crypto_pwhash отработал", rc == 0);
        ZT_TRUE("Argon2id совпал с эталоном",
                QByteArray(reinterpret_cast<char*>(out), sizeof(out)) ==
                    hex(kat.wantHex));
    }
}

}  // namespace

TEST(CryptoVectors, All) {
    ZT_TRUE("sodium_init", sodium_init() >= 0);
    checkHChaCha20();
    checkAead();
    checkArgon2id();
    EXPECT_EQ(0, zt::freshFailures());
}
