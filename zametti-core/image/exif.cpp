#include "exif.h"

#include <cstring>

namespace zametti {

namespace {

// --- чтение чисел ---------------------------------------------------------
//
// Порядок байт у контейнеров разный и не совпадает с порядком внутри EXIF:
// у JPEG и PNG длины big-endian, у RIFF little-endian, а внутри EXIF — как
// сказано в его собственном заголовке. Поэтому каждая функция берёт порядок
// явным параметром, а не полагается на «как обычно».

uint16_t be16(const uint8_t* p) { return uint16_t(p[0]) << 8 | p[1]; }
uint32_t be32(const uint8_t* p) {
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
uint32_t le32(const uint8_t* p) {
    return uint32_t(p[3]) << 24 | uint32_t(p[2]) << 16 | uint32_t(p[1]) << 8 | p[0];
}

uint16_t read16(const uint8_t* p, bool big) { return big ? be16(p) : uint16_t(p[1]) << 8 | p[0]; }
uint32_t read32(const uint8_t* p, bool big) { return big ? be32(p) : le32(p); }

void write16(uint8_t* p, uint16_t v, bool big) {
    if (big) {
        p[0] = uint8_t(v >> 8);
        p[1] = uint8_t(v);
    } else {
        p[0] = uint8_t(v);
        p[1] = uint8_t(v >> 8);
    }
}

const uint8_t* bytes(std::string_view s) {
    return reinterpret_cast<const uint8_t*>(s.data());
}

// --- EXIF -----------------------------------------------------------------

constexpr uint16_t kOrientationTag = 274;   // 0x0112
constexpr uint16_t kTypeShort = 3;

// Заголовок TIFF: "II" + 42 (little-endian) или "MM" + 42 (big-endian), затем
// смещение первого каталога.
bool exifHeader(std::string_view exif, bool* big, uint32_t* ifd0) {
    if (exif.size() < 8) return false;
    const uint8_t* p = bytes(exif);
    if (p[0] == 'I' && p[1] == 'I') *big = false;
    else if (p[0] == 'M' && p[1] == 'M') *big = true;
    else return false;
    if (read16(p + 2, *big) != 42) return false;
    *ifd0 = read32(p + 4, *big);
    // Каталог не может начинаться раньше самого заголовка, а его запись —
    // двенадцать байт плюс счётчик; всё, что не влезает, — мусор.
    if (*ifd0 < 8 || *ifd0 + 2 > exif.size()) return false;
    return true;
}

// Смещение ЗНАЧЕНИЯ тега в первом каталоге, или 0. Значение записи лежит после
// её типа и числа элементов, то есть на восемь байт дальше начала записи.
size_t findTagValue(std::string_view exif, uint16_t tag, bool* bigOut) {
    bool big = false;
    uint32_t ifd0 = 0;
    if (!exifHeader(exif, &big, &ifd0)) return 0;
    *bigOut = big;

    const uint8_t* p = bytes(exif);
    const uint16_t count = read16(p + ifd0, big);
    for (uint16_t i = 0; i < count; ++i) {
        const size_t entry = size_t(ifd0) + 2 + size_t(i) * 12;
        if (entry + 12 > exif.size()) return 0;
        if (read16(p + entry, big) == tag) return entry + 8;
    }
    return 0;
}

// Запись тега в ЛЮБОМ каталоге, а не только в нулевом: дата съёмки лежит в
// подкаталоге Exif IFD, и ходить туда прежним findTagValue было нечем.
// Возвращает смещение самой записи (12 байт), или 0.
size_t findEntryIn(std::string_view exif, uint32_t ifd, uint16_t tag, bool big) {
    if (ifd < 8 || size_t(ifd) + 2 > exif.size()) return 0;
    const uint8_t* p = bytes(exif);
    const uint16_t count = read16(p + ifd, big);
    for (uint16_t i = 0; i < count; ++i) {
        const size_t entry = size_t(ifd) + 2 + size_t(i) * 12;
        if (entry + 12 > exif.size()) return 0;
        if (read16(p + entry, big) == tag) return entry;
    }
    return 0;
}

// ASCII-значение записи. Значения длиннее четырёх байт лежат НЕ в самой
// записи, а по смещению из неё, — на этом молча ошибаются чаще всего.
std::string asciiValue(std::string_view exif, size_t entry, bool big) {
    if (entry == 0 || entry + 12 > exif.size()) return {};
    const uint8_t* p = bytes(exif);
    const uint16_t type = read16(p + entry + 2, big);
    if (type != 2 && type != 7) return {};   // ASCII либо UNDEFINED (UserComment)
    const uint32_t count = read32(p + entry + 4, big);
    if (count == 0 || count > exif.size()) return {};
    size_t at = entry + 8;
    if (count > 4) {
        const uint32_t offset = read32(p + entry + 8, big);
        if (offset + count > exif.size()) return {};
        at = offset;
    }
    std::string_view value = exif.substr(at, count);
    // UserComment начинается с восьмибайтовой пометки кодировки ("ASCII\0\0\0",
    // "UNICODE\0"). Её надо снять, иначе в подпись уедет служебное слово.
    if (type == 7 && value.size() > 8) {
        if (value.compare(0, 6, "ASCII\0", 6) == 0) value = value.substr(8);
        else return {};   // UNICODE/JIS не разбираем: там UTF-16 и своя возня
    }
    while (!value.empty() && (value.back() == '\0' || value.back() == ' '))
        value.remove_suffix(1);
    while (!value.empty() && value.front() == ' ') value.remove_prefix(1);
    return std::string(value);
}

// Подкаталог Exif IFD: тег 0x8769 нулевого каталога, значение — смещение.
uint32_t exifSubIfd(std::string_view exif, uint32_t ifd0, bool big) {
    const size_t entry = findEntryIn(exif, ifd0, 0x8769, big);
    if (entry == 0) return 0;
    return read32(bytes(exif) + entry + 8, big);
}

// "YYYY:MM:DD HH:MM:SS" из EXIF — в "YYYY-MM-DDTHH:MM:SS".
std::string isoFromExifTime(const std::string& raw) {
    if (raw.size() < 19) return {};
    std::string out = raw.substr(0, 19);
    if (out[4] != ':' || out[7] != ':' || out[10] != ' ') return {};
    out[4] = '-';
    out[7] = '-';
    out[10] = 'T';
    return out;
}

// --- обход контейнеров ----------------------------------------------------

// Длины ЯВНО: string_view из литерала обрывается на первом нуле, и "Exif\0\0"
// молча дал бы четыре байта вместо шести. На этом я и наступил — блоб уезжал
// на два байта, и заголовок TIFF переставал опознаваться.
constexpr std::string_view kExifPrefix{"Exif\0\0", 6};                  // APP1
constexpr std::string_view kXmpPrefix{"http://ns.adobe.com/xap/1.0/", 28};  // APP1
constexpr std::string_view kIccPrefix{"ICC_PROFILE\0", 12};             // APP2

bool starts(std::string_view s, std::string_view prefix) {
    return s.size() >= prefix.size() && std::memcmp(s.data(), prefix.data(), prefix.size()) == 0;
}

}  // namespace

bool looksLikeExif(std::string_view exif) {
    bool big = false;
    uint32_t ifd0 = 0;
    return exifHeader(exif, &big, &ifd0);
}

std::string exifDateTaken(std::string_view exif) {
    bool big = false;
    uint32_t ifd0 = 0;
    if (!exifHeader(exif, &big, &ifd0)) return {};

    // Момент СЪЁМКИ, а не правки: DateTimeOriginal живёт в подкаталоге Exif.
    if (const uint32_t sub = exifSubIfd(exif, ifd0, big)) {
        const std::string taken = asciiValue(exif, findEntryIn(exif, sub, 0x9003, big), big);
        const std::string iso = isoFromExifTime(taken);
        if (!iso.empty()) return iso;
    }
    // Запасной ход — DateTime из нулевого каталога: у сканов и правленых
    // файлов другого времени и нет.
    return isoFromExifTime(asciiValue(exif, findEntryIn(exif, ifd0, 0x0132, big), big));
}

std::string exifComment(std::string_view exif) {
    bool big = false;
    uint32_t ifd0 = 0;
    if (!exifHeader(exif, &big, &ifd0)) return {};
    if (const uint32_t sub = exifSubIfd(exif, ifd0, big)) {
        const std::string user = asciiValue(exif, findEntryIn(exif, sub, 0x9286, big), big);
        if (!user.empty()) return user;
    }
    return asciiValue(exif, findEntryIn(exif, ifd0, 0x010E, big), big);
}

Orientation exifOrientation(std::string_view exif) {
    bool big = false;
    const size_t at = findTagValue(exif, kOrientationTag, &big);
    if (at == 0 || at + 2 > exif.size()) return Orientation::Normal;
    // Тип обязан быть SHORT: если в файле лежит что-то другое, значение читать
    // нельзя — можно принять за поворот кусок совсем другого тега.
    const size_t entry = at - 8;
    if (read16(bytes(exif) + entry + 2, big) != kTypeShort) return Orientation::Normal;
    const uint16_t v = read16(bytes(exif) + at, big);
    if (v < 1 || v > 8) return Orientation::Normal;   // 0 пишут телефоны vivo
    return static_cast<Orientation>(v);
}

bool resetExifOrientation(std::string* exif) {
    if (!exif) return false;
    bool big = false;
    const size_t at = findTagValue(*exif, kOrientationTag, &big);
    if (at == 0 || at + 2 > exif->size()) return false;
    const size_t entry = at - 8;
    if (read16(bytes(*exif) + entry + 2, big) != kTypeShort) return false;
    write16(reinterpret_cast<uint8_t*>(exif->data()) + at, 1, big);
    return true;
}

// --- JPEG -----------------------------------------------------------------
//
// Обход маркеров. ICC в JPEG бывает разрезан на куски по 64 КБ, и каждый несёт
// свой номер и общее число — собираем в порядке номеров, а не в порядке
// встречи: порядок кусков в файле стандартом не закреплён.

bool readJpegMeta(std::string_view file, ImageMeta* out) {
    if (!out) return false;
    if (file.size() < 4) return false;
    const uint8_t* p = bytes(file);
    if (p[0] != 0xFF || p[1] != 0xD8) return false;   // не SOI — не JPEG

    std::string iccChunks[256];
    int iccTotal = 0;

    size_t i = 2;
    while (i + 4 <= file.size() && p[i] == 0xFF) {
        const uint8_t marker = p[i + 1];
        if (marker == 0xD8 || marker == 0xD9 || (marker >= 0xD0 && marker <= 0xD7)) {
            i += 2;
            continue;
        }
        if (marker == 0xDA) break;   // начались данные — метаданных дальше нет
        const size_t len = be16(p + i + 2);
        if (len < 2 || i + 2 + len > file.size()) break;
        const std::string_view seg(file.data() + i + 4, len - 2);

        if (marker == 0xE1) {
            if (starts(seg, kExifPrefix) && out->exif.empty()) {
                out->exif.assign(seg.data() + kExifPrefix.size(), seg.size() - kExifPrefix.size());
            } else if (starts(seg, kXmpPrefix) && out->xmp.empty()) {
                // После заголовка идёт нулевой байт, потом сам XML.
                const size_t skip = kXmpPrefix.size() + 1;
                if (seg.size() > skip) out->xmp.assign(seg.data() + skip, seg.size() - skip);
            }
        } else if (marker == 0xE2 && starts(seg, kIccPrefix)) {
            // За именем идут номер куска и их общее число, оба с единицы.
            const size_t head = kIccPrefix.size() + 2;
            if (seg.size() > head) {
                const uint8_t no = uint8_t(seg[kIccPrefix.size()]);
                const uint8_t total = uint8_t(seg[kIccPrefix.size() + 1]);
                if (no >= 1 && total >= 1 && no <= total) {
                    iccChunks[no].assign(seg.data() + head, seg.size() - head);
                    iccTotal = total;
                }
            }
        }
        i += 2 + len;
    }

    if (iccTotal > 0) {
        for (int n = 1; n <= iccTotal; ++n) out->icc += iccChunks[n];
    }
    out->orientation = exifOrientation(out->exif);
    return true;
}

// --- WebP -----------------------------------------------------------------
//
// RIFF: "RIFF" + размер + "WEBP", дальше чанки «имя + размер + данные», каждый
// выровнен до чётной длины. Метаданные лежат в EXIF, XMP и ICCP.

bool readWebpMeta(std::string_view file, ImageMeta* out) {
    if (!out) return false;
    if (file.size() < 12) return false;
    const uint8_t* p = bytes(file);
    if (std::memcmp(p, "RIFF", 4) != 0 || std::memcmp(p + 8, "WEBP", 4) != 0) return false;

    size_t i = 12;
    while (i + 8 <= file.size()) {
        const std::string_view name(file.data() + i, 4);
        const uint32_t len = le32(p + i + 4);
        const size_t body = i + 8;
        if (len > file.size() || body + len > file.size()) break;
        if (name == "EXIF" && out->exif.empty()) {
            out->exif.assign(file.data() + body, len);
        } else if (name == "XMP " && out->xmp.empty()) {
            out->xmp.assign(file.data() + body, len);
        } else if (name == "ICCP" && out->icc.empty()) {
            out->icc.assign(file.data() + body, len);
        }
        i = body + len + (len & 1);   // выравнивание до чётного
    }
    out->orientation = exifOrientation(out->exif);
    return true;
}

// --- PNG ------------------------------------------------------------------
//
// Чанки «размер + имя + данные + контрольная сумма». EXIF лежит в eXIf (с
// версии 1.5), XMP — в iTXt с ключом XML:com.adobe.xmp, профиль — в iCCP,
// сжатый deflate. Профиль отсюда НЕ достаём: разжимать его пришлось бы zlib,
// которая живёт двумя слоями выше и ядру не видна. Цветовое пространство PNG
// нам и так приносит Qt через QImage::colorSpace.

bool readPngMeta(std::string_view file, ImageMeta* out) {
    if (!out) return false;
    if (file.size() < 8) return false;
    static const uint8_t kSig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    const uint8_t* p = bytes(file);
    if (std::memcmp(p, kSig, 8) != 0) return false;

    static constexpr std::string_view kXmpKey = "XML:com.adobe.xmp";
    size_t i = 8;
    while (i + 12 <= file.size()) {
        const uint32_t len = be32(p + i);
        const std::string_view name(file.data() + i + 4, 4);
        const size_t body = i + 8;
        if (len > file.size() || body + len + 4 > file.size()) break;
        if (name == "eXIf" && out->exif.empty()) {
            out->exif.assign(file.data() + body, len);
        } else if (name == "iTXt" && out->xmp.empty()) {
            const std::string_view chunk(file.data() + body, len);
            if (starts(chunk, kXmpKey)) {
                // ключ \0 сжатие метод \0 язык \0 перевод ключа \0 текст
                size_t at = kXmpKey.size() + 1;
                if (at + 2 <= chunk.size() && chunk[at] == 0) {
                    at += 2;                       // флаг сжатия и метод
                    int zeros = 0;                 // язык и перевод ключа
                    while (at < chunk.size() && zeros < 2) {
                        if (chunk[at] == 0) ++zeros;
                        ++at;
                    }
                    if (at < chunk.size()) out->xmp.assign(chunk.data() + at, chunk.size() - at);
                }
            }
        } else if (name == "IDAT") {
            break;   // дальше пиксели; eXIf по стандарту стоит до них
        }
        i = body + len + 4;   // + контрольная сумма
    }
    out->orientation = exifOrientation(out->exif);
    return true;
}

bool readJxlMeta(std::string_view file, ImageMeta* out) {
    if (out == nullptr) return false;
    static constexpr std::string_view kSignature{"\0\0\0\x0CJXL \r\n\x87\n", 12};
    if (!starts(file, kSignature)) return false;

    size_t at = 0;
    while (at + 8 <= file.size()) {
        uint64_t size = read32(bytes(file) + at, true);
        const std::string_view type = file.substr(at + 4, 4);
        size_t header = 8;
        if (size == 1) {   // расширенный размер: 64 бита после типа
            if (at + 16 > file.size()) break;
            size = (uint64_t(read32(bytes(file) + at + 8, true)) << 32) |
                   read32(bytes(file) + at + 12, true);
            header = 16;
        } else if (size == 0) {
            size = file.size() - at;   // «до конца файла»
        }
        if (size < header || at + size > file.size()) break;
        const std::string_view body = file.substr(at + header, size - header);
        // Бокс Exif начинается с ЧЕТЫРЁХ НУЛЕЙ — смещения до заголовка TIFF
        // (спецификация JXL, приложение о боксах). Дальше идёт обычный блоб,
        // такой же, как в APP1 у JPEG.
        if (type == "Exif" && body.size() > 4) out->exif = std::string(body.substr(4));
        else if (type == "xml ") out->xmp = std::string(body);
        at += size;
    }
    if (!out->exif.empty()) out->orientation = exifOrientation(out->exif);
    return !out->exif.empty() || !out->xmp.empty();
}

ImageMeta readImageMeta(std::string_view file) {
    ImageMeta meta;
    if (file.size() >= 12) {
        const uint8_t* p = bytes(file);
        if (p[0] == 0xFF && p[1] == 0xD8) {
            readJpegMeta(file, &meta);
        } else if (std::memcmp(p, "RIFF", 4) == 0 && std::memcmp(p + 8, "WEBP", 4) == 0) {
            readWebpMeta(file, &meta);
        } else if (p[0] == 0x89 && std::memcmp(p + 1, "PNG", 3) == 0) {
            readPngMeta(file, &meta);
        } else {
            readJxlMeta(file, &meta);
        }
        // TIFF и HEIF сюда не попадают намеренно: у каждого метаданные забирает
        // его собственный читатель.
    }
    return meta;
}

// --- сверка двойников -----------------------------------------------------

namespace {

// Сравнение накопленных кусков. Собираем в строки, а не сравниваем на лету:
// куски одних и тех же данных в двух файлах бывают нарезаны по-разному
// (цепочка jxlp против одного jxlc), и «по кускам» они не сойдутся, хотя сами
// данные равны.
bool sameBytes(const std::string& a, const std::string& b) {
    return !a.empty() && a == b;
}

// JXL: голый кодовый поток (подпись FF 0A) или контейнер ISO BMFF.
std::string jxlData(std::string_view file) {
    if (file.size() >= 2 && uint8_t(file[0]) == 0xFF && uint8_t(file[1]) == 0x0A)
        return std::string(file);   // голый поток: он весь и есть данные

    static constexpr std::string_view kSignature{"\0\0\0\x0CJXL \r\n\x87\n", 12};
    if (!starts(file, kSignature)) return {};

    std::string data;
    size_t at = 0;
    while (at + 8 <= file.size()) {
        uint64_t size = read32(bytes(file) + at, true);
        const std::string_view type = file.substr(at + 4, 4);
        size_t header = 8;
        if (size == 1) {   // расширенный размер: 64 бита после типа
            if (at + 16 > file.size()) return {};
            size = (uint64_t(read32(bytes(file) + at + 8, true)) << 32) |
                   read32(bytes(file) + at + 12, true);
            header = 16;
        } else if (size == 0) {
            size = file.size() - at;   // «до конца файла»
        }
        if (size < header || at + size > file.size()) return {};
        if (type == "jxlc" || type == "jxlp") {
            std::string_view body = file.substr(at + header, size - header);
            // У jxlp первые четыре байта — номер куска, а не данные.
            if (type == "jxlp" && body.size() >= 4) body = body.substr(4);
            data.append(body);
        }
        at += size;
    }
    return data;
}

// WebP: чанки RIFF. Данные — VP8 (lossy), VP8L (lossless), ALPH (альфа);
// метаданные — EXIF, XMP, ICCP; VP8X — заголовок расширенного контейнера, в
// нём лежат флаги наличия метаданных, и сравнивать его нельзя.
std::string webpData(std::string_view file) {
    if (file.size() < 12 || !starts(file, "RIFF") || file.substr(8, 4) != "WEBP") return {};
    std::string data;
    size_t at = 12;
    while (at + 8 <= file.size()) {
        const std::string_view type = file.substr(at, 4);
        const uint32_t size = read32(bytes(file) + at + 4, false);
        if (at + 8 + size > file.size()) return {};
        if (type == "VP8 " || type == "VP8L" || type == "ALPH")
            data.append(file.substr(at + 8, size));
        at += 8 + size + (size & 1);   // чанки выровнены по чётному байту
    }
    return data;
}

// JPEG: маркеры. Данные — всё, кроме APP1 (Exif, XMP), APP2 (ICC) и COM.
// Остальные APP-маркеры (JFIF, Adobe) оставляем: они влияют на разжатие.
std::string jpegData(std::string_view file) {
    if (file.size() < 4 || uint8_t(file[0]) != 0xFF || uint8_t(file[1]) != 0xD8) return {};
    std::string data;
    size_t at = 2;
    while (at + 4 <= file.size()) {
        if (uint8_t(file[at]) != 0xFF) return {};
        const uint8_t marker = uint8_t(file[at + 1]);
        if (marker == 0xD9) break;                       // конец
        const size_t length = read16(bytes(file) + at + 2, true);
        if (length < 2 || at + 2 + length > file.size()) return {};
        const bool meta = marker == 0xE1 || marker == 0xE2 || marker == 0xFE;
        if (!meta) data.append(file.substr(at, 2 + length));
        at += 2 + length;
        if (marker == 0xDA) {   // начало скана: дальше сжатые данные до конца
            data.append(file.substr(at));
            break;
        }
    }
    return data;
}

}  // namespace

bool sameJxlCompressedData(std::string_view a, std::string_view b) {
    return sameBytes(jxlData(a), jxlData(b));
}

bool sameWebpCompressedData(std::string_view a, std::string_view b) {
    return sameBytes(webpData(a), webpData(b));
}

bool sameJpegCompressedData(std::string_view a, std::string_view b) {
    return sameBytes(jpegData(a), jpegData(b));
}

bool sameCompressedData(std::string_view a, std::string_view b) {
    if (sameJxlCompressedData(a, b)) return true;
    if (sameWebpCompressedData(a, b)) return true;
    return sameJpegCompressedData(a, b);
}

bool webpIsLossless(std::string_view file) {
    if (file.size() < 12) return false;
    const uint8_t* p = bytes(file);
    if (std::memcmp(p, "RIFF", 4) != 0 || std::memcmp(p + 8, "WEBP", 4) != 0) return false;

    size_t i = 12;
    while (i + 8 <= file.size()) {
        const std::string_view name(file.data() + i, 4);
        const uint32_t len = le32(p + i + 4);
        const size_t body = i + 8;
        if (len > file.size() || body + len > file.size()) break;
        // VP8L — пиксели закодированы без потерь. VP8 (с пробелом) — с
        // потерями. VP8X только объявляет расширения, ответа в нём нет, и мы
        // идём дальше по чанкам.
        if (name == "VP8L") return true;
        if (name == "VP8 ") return false;
        i = body + len + (len & 1);
    }
    return false;
}

namespace {

// Экранирование для XML. Имя файла — чужая строка, в ней бывает и «&», и «<»;
// без этого один такой файл сделал бы XMP невалидным, а метаданные —
// нечитаемыми целиком.
std::string escapeXml(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 16);
    for (char c : text) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&apos;"; break;
            default: out.push_back(c);
        }
    }
    return out;
}

}  // namespace

namespace {

// Значение свойства XMP: либо элементом <ns:имя>значение</ns:имя>, либо
// атрибутом ns:имя="значение". Оба вида встречаются в живых файлах, и который
// именно — зависит от программы, которая писала.
std::string xmpValue(std::string_view xmp, std::string_view name) {
    const std::string element = "<" + std::string(name) + ">";
    size_t at = xmp.find(element);
    if (at != std::string_view::npos) {
        const size_t from = at + element.size();
        const size_t to = xmp.find('<', from);
        if (to != std::string_view::npos) return std::string(xmp.substr(from, to - from));
    }
    const std::string attribute = std::string(name) + "=\"";
    at = xmp.find(attribute);
    if (at == std::string_view::npos) return {};
    const size_t from = at + attribute.size();
    const size_t to = xmp.find('"', from);
    if (to == std::string_view::npos) return {};
    return std::string(xmp.substr(from, to - from));
}

}  // namespace

std::string xmpDescription(std::string_view xmp) {
    // dc:description чаще всего обёрнут в rdf:Alt/rdf:li — берём внутренность.
    std::string value = xmpValue(xmp, "rdf:li");
    if (xmp.find("dc:description") == std::string_view::npos) value.clear();
    if (value.empty()) value = xmpValue(xmp, "dc:description");
    return value;
}

std::string xmpCreateDate(std::string_view xmp) {
    std::string value = xmpValue(xmp, "xmp:CreateDate");
    // Зону и доли секунды отбрасываем: нам нужна секунда, из которой чеканится
    // имя, и она обязана совпасть с той, что даёт EXIF.
    if (value.size() > 19) value.resize(19);
    return value;
}

std::string xmpWithFileName(std::string_view existing, std::string_view fileName) {
    if (fileName.empty()) return std::string(existing);

    const std::string block =
        "  <rdf:Description rdf:about=\"\""
        " xmlns:xmpMM=\"http://ns.adobe.com/xap/1.0/mm/\">\n"
        "   <xmpMM:PreservedFileName>" + escapeXml(fileName) +
        "</xmpMM:PreservedFileName>\n"
        "  </rdf:Description>\n";

    // Если XMP уже есть — вставляем свой Description перед закрытием rdf:RDF.
    // Ищем именно закрывающий тег, а не разбираем XML: разборщик XML ради
    // одного поля — цена, которой это не стоит.
    const std::string_view close = "</rdf:RDF>";
    const size_t at = existing.rfind(close);
    if (at != std::string_view::npos) {
        std::string out;
        out.reserve(existing.size() + block.size());
        out.append(existing.substr(0, at));
        out.append(block);
        out.append(existing.substr(at));
        return out;
    }

    // Не нашли — собираем пакет с нуля. Заголовок и хвост ровно те, что велит
    // спецификация XMP; id в xpacket постоянный и тоже задан ею.
    return
        "<?xpacket begin=\"\xEF\xBB\xBF\" id=\"W5M0MpCehiHzreSzNTczkc9d\"?>\n"
        "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\">\n"
        " <rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">\n" +
        block +
        " </rdf:RDF>\n"
        "</x:xmpmeta>\n"
        "<?xpacket end=\"w\"?>";
}

}  // namespace zametti
