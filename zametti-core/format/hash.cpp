#include "hash.h"

#include "blake3.h"

namespace zametti {

bool Digest::empty() const {
    for (uint8_t b : bytes)
        if (b != 0) return false;
    return true;
}

std::string Digest::hex() const {
    static const char* const kDigits = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (uint8_t b : bytes) {
        out.push_back(kDigits[b >> 4]);
        out.push_back(kDigits[b & 0x0f]);
    }
    return out;
}

Digest hashOf(std::string_view content) {
    blake3_hasher hasher;
    blake3_hasher_init(&hasher);
    // Пустой вход — законный: у BLAKE3 есть отпечаток и у него, и он не нулевой,
    // так что «пусто» и «не считали» не путаются.
    if (!content.empty()) blake3_hasher_update(&hasher, content.data(), content.size());
    Digest digest;
    blake3_hasher_finalize(&hasher, digest.bytes.data(), digest.bytes.size());
    return digest;
}

}  // namespace zametti
