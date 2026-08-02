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

// Имя исходника в XMP. Не «функция что-то вернула», а свойства, на которых
// стоит польза: имя должно найтись в готовом XMP, чужие поля должны уцелеть, а
// XML — не развалиться от знака «&» в имени.
void checkFileNameInXmp() {
    // Пустого XMP не было — собираем пакет с нуля.
    const std::string fresh = xmpWithFileName({}, "фото.jpg");
    ZT_TRUE("собран пакет XMP", fresh.find("<x:xmpmeta") != std::string::npos &&
                                fresh.find("</x:xmpmeta>") != std::string::npos);
    ZT_TRUE("имя на месте", fresh.find("<xmpMM:PreservedFileName>фото.jpg<") !=
                                std::string::npos);
    ZT_TRUE("объявлено пространство имён xmpMM",
            fresh.find("http://ns.adobe.com/xap/1.0/mm/") != std::string::npos);

    // XMP уже был — чужое обязано уцелеть целиком.
    const std::string had =
        "<?xpacket begin=\"\" id=\"W5M0MpCehiHzreSzNTczkc9d\"?>\n"
        "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\">\n"
        " <rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">\n"
        "  <rdf:Description rdf:about=\"\" xmlns:dc=\"http://purl.org/dc/elements/1.1/\">\n"
        "   <dc:creator>Кто-то другой</dc:creator>\n"
        "  </rdf:Description>\n"
        " </rdf:RDF>\n"
        "</x:xmpmeta>\n<?xpacket end=\"w\"?>";
    const std::string merged = xmpWithFileName(had, "снимок.png");
    ZT_TRUE("чужое поле уцелело", merged.find("<dc:creator>Кто-то другой</dc:creator>") !=
                                      std::string::npos);
    ZT_TRUE("и имя добавилось", merged.find("<xmpMM:PreservedFileName>снимок.png<") !=
                                    std::string::npos);
    ZT_TRUE("пакет остался одним целым",
            merged.find("</rdf:RDF>") != std::string::npos &&
                merged.find("</rdf:RDF>") == merged.rfind("</rdf:RDF>"));
    // Наш блок обязан лежать ВНУТРИ rdf:RDF, иначе это не XMP, а мусор рядом.
    ZT_TRUE("вставлено внутрь rdf:RDF",
            merged.find("PreservedFileName") < merged.find("</rdf:RDF>"));

    // Знаки, ломающие XML. Один такой файл сделал бы метаданные нечитаемыми
    // целиком — не только имя.
    const std::string tricky = xmpWithFileName({}, "a&b<c>d\"e.jpg");
    ZT_TRUE("амперсанд экранирован", tricky.find("a&amp;b") != std::string::npos);
    ZT_TRUE("угловые скобки экранированы", tricky.find("&lt;c&gt;") != std::string::npos);
    ZT_TRUE("сырых скобок в имени не осталось",
            tricky.find("b<c") == std::string::npos);

    // Пустое имя — не повод портить существующий XMP.
    ZT_EQ("пустое имя ничего не меняет", had, xmpWithFileName(had, {}));
}

// Различитель вида сжатия WebP. От него зависит, с чем состязается пережатие:
// у lossless-исходника есть право остаться точным, у lossy — нет.
void checkWebpFlavour(const std::filesystem::path& root) {
    const auto read = [](const std::filesystem::path& p) {
        std::ifstream in(p, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    };
    const auto lossless = root / "formats" / "плоская-графика-lossless.webp";
    const auto lossy = root / "formats" / "плоская-графика-lossy.webp";
    if (!std::filesystem::exists(lossless) || !std::filesystem::exists(lossy)) return;

    // Один и тот же рисунок, сохранённый двумя способами: расширение у них
    // одинаковое, и отличить их можно только по чанку внутри контейнера.
    ZT_TRUE("VP8L опознан как lossless", webpIsLossless(read(lossless)));
    ZT_TRUE("VP8 опознан как lossy", !webpIsLossless(read(lossy)));

    // Мусор и обрывки не должны выдавать себя за lossless.
    ZT_TRUE("пустое — не lossless", !webpIsLossless(std::string_view()));
    ZT_TRUE("не webp — не lossless", !webpIsLossless(std::string_view("RIFF____NOTW", 12)));
    const std::string cut = read(lossless).substr(0, 14);
    ZT_TRUE("обрывок — не lossless", !webpIsLossless(cut));
}

}  // namespace

int main(int argc, char** argv) {
    checkOrientationParsing();
    checkOrientationReset();
    checkFileNameInXmp();
    if (argc > 1) checkWebpFlavour(std::filesystem::path(argv[1]));
    if (argc > 1) checkRealFiles(std::filesystem::path(argv[1]));
    return zt::report("метаданные картинок");
}
