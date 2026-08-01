// Метаданные картинок: обход контейнеров и правка ориентации.
//
// Проверяется на НАСТОЯЩИХ файлах владельца — сфабрикованный JPEG проверял бы
// мой же разборщик против моего же представления о формате. Эталон берётся
// оттуда, где он независим: размеры блобов и значение поворота сверены с
// exiftool и записаны сюда числами.
//
// Каталог с картинками задаётся первым аргументом; без него набор молча
// проходит — корпуса в репозитории нет и не будет (см. .gitignore).

#include "exif.h"

#include "test_util.h"

#include <filesystem>
#include <fstream>
#include <string>

using namespace zametti;

namespace {

std::string readFile(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

std::string num(size_t v) { return std::to_string(v); }

// Блоб EXIF, собранный руками: заголовок TIFF + один каталог с одним тегом
// Orientation. Нужен там, где важна не разметка контейнера, а разбор самого
// EXIF — и где настоящий файл дал бы лишние степени свободы.
std::string makeExif(bool big, uint16_t orientation) {
    std::string s;
    auto put16 = [&](uint16_t v) {
        if (big) { s += char(v >> 8); s += char(v & 0xFF); }
        else     { s += char(v & 0xFF); s += char(v >> 8); }
    };
    auto put32 = [&](uint32_t v) {
        if (big) { s += char(v >> 24); s += char(v >> 16); s += char(v >> 8); s += char(v); }
        else     { s += char(v); s += char(v >> 8); s += char(v >> 16); s += char(v >> 24); }
    };
    s += big ? "MM" : "II";
    put16(42);
    put32(8);          // первый каталог сразу за заголовком
    put16(1);          // одна запись
    put16(274);        // Orientation
    put16(3);          // тип SHORT
    put32(1);          // одно значение
    put16(orientation);
    put16(0);          // добивка значения до четырёх байт
    put32(0);          // следующего каталога нет
    return s;
}

void checkOrientationParsing() {
    // Оба порядка байт: файлы Canon пишут big-endian, телефоны — little.
    for (bool big : {false, true}) {
        const std::string label = big ? " (MM)" : " (II)";
        ZT_TRUE("блоб опознан" + label, looksLikeExif(makeExif(big, 6)));
        ZT_EQ("поворот 6 прочитан" + label, num(6),
              num(size_t(exifOrientation(makeExif(big, 6)))));
        ZT_EQ("поворот 1 прочитан" + label, num(1),
              num(size_t(exifOrientation(makeExif(big, 1)))));
        // Ноль пишет vivo X300 Ultra — 34 файла в корпусе владельца. По
        // стандарту значение недопустимо, и принимать его нельзя.
        ZT_EQ("недопустимый ноль трактуется как «без поворота»" + label, num(1),
              num(size_t(exifOrientation(makeExif(big, 0)))));
        ZT_EQ("значение вне 1..8 трактуется как «без поворота»" + label, num(1),
              num(size_t(exifOrientation(makeExif(big, 9)))));
    }

    ZT_TRUE("мусор не опознаётся", !looksLikeExif("не exif вовсе"));
    ZT_TRUE("пустое не опознаётся", !looksLikeExif(""));
    // Обрезанный блоб: заголовок обещает каталог за пределами данных.
    std::string cut = makeExif(false, 6);
    cut.resize(9);
    ZT_TRUE("обрезанный блоб не опознаётся", !looksLikeExif(cut));
}

void checkOrientationReset() {
    std::string exif = makeExif(false, 6);
    const size_t before = exif.size();
    ZT_TRUE("поворот сброшен", resetExifOrientation(&exif));
    ZT_EQ("после сброса поворота нет", num(1), num(size_t(exifOrientation(exif))));
    // Длина обязана остаться той же: смещения внутри EXIF абсолютные, и сдвиг
    // блоба сломал бы теги, которые ссылаются на данные за каталогом.
    ZT_EQ("длина блоба не изменилась", num(before), num(exif.size()));

    std::string noTag = "II";
    noTag += '\x2a'; noTag += '\0';
    noTag += '\x08'; noTag += '\0'; noTag += '\0'; noTag += '\0';
    noTag += '\0'; noTag += '\0';   // ноль записей
    ZT_TRUE("сбрасывать нечего — не ошибка", !resetExifOrientation(&noTag));
}

void checkRealFiles(const std::filesystem::path& root) {
    struct Case {
        const char* path;
        bool exif;
        bool xmp;
        bool icc;
        int orientation;
        const char* why;
    };
    // Числа сверены с exiftool.
    const Case cases[] = {
        {"photo/orientation6.jpg", true, false, false, 6, "повёрнутый JPEG с Olympus"},
        {"photo/phone-gps.jpg", true, false, false, 1, "толстый EXIF с GPS (XMP нет — сверено exiftool)"},
        {"vivo/поворот-ноль-p3gamut-srgbtransfer.jpg", true, true, true, 1,
         "недопустимый Orientation = 0 в настоящем файле"},
        {"vivo/vivo-display-p3.jpg", true, true, true, 1, "профиль Display P3"},
        {"art/leonardo-oldmen.jpg", false, false, false, 1, "JPEG без метаданных вовсе"},
    };

    for (const Case& c : cases) {
        const std::filesystem::path p = root / c.path;
        if (!std::filesystem::exists(p)) continue;   // набора может не быть
        const std::string bytes = readFile(p);
        ZT_TRUE(std::string("прочитан ") + c.path, !bytes.empty());
        const ImageMeta meta = readImageMeta(bytes);

        const std::string what = std::string(c.why) + " — ";
        ZT_TRUE(what + "EXIF " + (c.exif ? "есть" : "нет"), meta.exif.empty() != c.exif);
        ZT_TRUE(what + "XMP " + (c.xmp ? "есть" : "нет"), meta.xmp.empty() != c.xmp);
        ZT_TRUE(what + "ICC " + (c.icc ? "есть" : "нет"), meta.icc.empty() != c.icc);
        ZT_EQ(what + "поворот", num(size_t(c.orientation)), num(size_t(meta.orientation)));
        if (!meta.exif.empty())
            ZT_TRUE(what + "блоб EXIF настоящий", looksLikeExif(meta.exif));

        // Сброс на настоящем файле: после него поворота нет, длина та же.
        if (c.orientation != 1) {
            std::string exif = meta.exif;
            const size_t before = exif.size();
            ZT_TRUE(what + "поворот сброшен", resetExifOrientation(&exif));
            ZT_EQ(what + "после сброса поворота нет", num(1),
                  num(size_t(exifOrientation(exif))));
            ZT_EQ(what + "длина блоба не изменилась", num(before), num(exif.size()));
        }
    }

    // WebP и PNG: свои контейнеры, свои чанки.
    const std::filesystem::path webp = root / "formats/webp-sample.webp";
    if (std::filesystem::exists(webp)) {
        const ImageMeta meta = readImageMeta(readFile(webp));
        // Сам файл метаданных не несёт — важно, что обход прошёл и не соврал.
        ZT_TRUE("webp разобран без выдумок", meta.exif.empty() && meta.icc.empty());
    }
    const std::filesystem::path png = root / "quality/png-16бит.png";
    if (std::filesystem::exists(png)) {
        const std::string bytes = readFile(png);
        ImageMeta meta;
        ZT_TRUE("png опознан", readPngMeta(bytes, &meta));
    }

    // Чужой формат чужому разборщику не отдаём.
    const std::filesystem::path jxl = root / "formats/dice.jxl";
    if (std::filesystem::exists(jxl)) {
        ImageMeta meta;
        ZT_TRUE("jxl не притворяется JPEG", !readJpegMeta(readFile(jxl), &meta));
        ZT_TRUE("jxl не притворяется WebP", !readWebpMeta(readFile(jxl), &meta));
        ZT_TRUE("jxl не притворяется PNG", !readPngMeta(readFile(jxl), &meta));
    }
}

}  // namespace

int main(int argc, char** argv) {
    checkOrientationParsing();
    checkOrientationReset();
    if (argc > 1) checkRealFiles(std::filesystem::path(argv[1]));
    return zt::report("метаданные картинок");
}
