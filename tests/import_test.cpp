// Таблица решений конвейера: каждый ряд на настоящем файле.
//
// Проверяется не «сжалось», а КАКИМ ПУТЁМ пошло. «Сжалось» само по себе не
// значит ничего: картинка, которая должна была лечь байт в байт транскодом, а
// пошла путём фото, тоже «сжалась» — только потеряла при этом качество
// необратимо.

#include "import.h"

#include "test_util.h"

#include <QBuffer>
#include <QColorSpace>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QImageReader>

#include <string>

using namespace zametti;

namespace {

std::string num(long long v) { return std::to_string(v); }

QImage decodeResult(const ImportResult& r) {
    QBuffer buf;
    buf.setData(r.bytes);
    buf.open(QIODevice::ReadOnly);
    QImageReader reader(&buf);
    return reader.read();
}

void expectRoute(const QString& root, const char* rel, Route want, const char* why,
                 const ImportLimits& limits) {
    const QString path = root + "/" + rel;
    if (!QFileInfo::exists(path)) return;
    const ImportResult r = importImage(path, limits);
    const std::string what = std::string(why) + " — ";
    ZT_EQ(what + "путь", std::string(routeName(want)), std::string(routeName(r.route)));
    if (r.route == Route::Refused) {
        ZT_TRUE(what + "у отказа есть внятная причина", !r.message.isEmpty());
        return;
    }
    ZT_TRUE(what + "байты есть", !r.bytes.isEmpty());
    ZT_TRUE(what + "расширение названо", !r.extension.isEmpty());
    // Апскейла нет ни на каком пути — это правило, и оно проверяется на каждом
    // файле, а не отдельным случаем.
    QImageReader probe(path);
    probe.setAutoTransform(true);
    const QSize src = probe.size();
    if (src.isValid() && r.size.width > 0) {
        ZT_TRUE(what + "не растянуто по ширине", r.size.width <= src.width());
        ZT_TRUE(what + "не растянуто по высоте", r.size.height <= src.height());
    }
}

void checkTable(const QString& root) {
    ImportLimits limits;   // умолчания: S=1920, качество 88, файл 1 МБ

    // --- ряд «JPEG влезает в оба бюджета» → байт-точный транскод ---------
    expectRoute(root, "art/leonardo-oldmen.jpg", Route::TranscodedJpeg,
                "небольшой JPEG влезает в оба бюджета", limits);
    expectRoute(root, "art/leonardo-tiny.jpg", Route::TranscodedJpeg, "крошечный JPEG", limits);

    // --- тот же ряд, но файл велик → путь фото ---------------------------
    expectRoute(root, "photo/sony-60mp.jpg", Route::Photo,
                "60 Мп не влезает ни по пикселям, ни по файлу", limits);
    expectRoute(root, "photo/wallpaper-4mp.jpg", Route::Photo,
                "4 Мп не влезает по пикселям", limits);

    // --- ряд «JXL» → как есть --------------------------------------------
    expectRoute(root, "formats/dice.jxl", Route::AsIs, "JXL влезает — кладём как есть", limits);

    // --- ряд «скриншот» → lossless ---------------------------------------
    expectRoute(root, "screen/screenshot-alpha.png", Route::Lossless,
                "скриншот: проба уводит в lossless", limits);

    // --- ряд «фотография в PNG» → путь фото ------------------------------
    expectRoute(root, "quality/png-16бит.png", Route::Photo,
                "фотография в PNG: проба уводит на путь фото", limits);

    // --- сырые форматы → честный отказ -----------------------------------
    for (const char* raw : {"raw/canon.cr3", "raw/olympus.orf", "raw/sony.arw",
                            "vivo/proraw.dng"}) {
        const QString path = root + "/" + raw;
        if (!QFileInfo::exists(path)) continue;
        const ImportResult r = importImage(path, limits);
        ZT_EQ(std::string("сырой формат отвергнут: ") + raw, std::string("отказ"),
              std::string(routeName(r.route)));
        ZT_TRUE(std::string("и сказано почему: ") + raw, !r.message.isEmpty());
    }
}

void checkTiffPath(const QString& root) {
    ImportLimits limits;
    // Lab-TIFF: главное — что он вообще прошёл и что цвет пересчитан нами, а
    // не взят у Qt.
    const QString lab = root + "/museum/lab-lzw.tif";
    if (!QFileInfo::exists(lab)) return;
    const ImportResult r = importImage(lab, limits);
    ZT_EQ("Lab-TIFF идёт путём фото", std::string("путь фото"),
          std::string(routeName(r.route)));
    const QImage back = decodeResult(r);
    ZT_TRUE("и читается обратно", !back.isNull());
    if (!back.isNull()) {
        // Сверяем СРЕДНИЙ цвет по всей картинке, а не отдельный пиксель:
        // положение пикселя после уменьшения смещается, а среднее — нет.
        //
        // Число независимое: у верного разбора этого файла среднее выходит
        // холодным (синего больше красного, 141/148/153), а у ошибки Qt —
        // тёплым (155/149/136). Направление перекоса и проверяем: оно
        // противоположно, и спутать нельзя.
        const QImage rgb = back.convertToFormat(QImage::Format_RGB888);
        double sum[3] = {0, 0, 0};
        for (int y = 0; y < rgb.height(); ++y)
            for (int x = 0; x < rgb.width(); ++x)
                for (int c = 0; c < 3; ++c)
                    sum[c] += rgb.constScanLine(y)[size_t(x) * 3 + size_t(c)];
        const double n = double(rgb.width()) * rgb.height();
        const int r = int(sum[0] / n), g = int(sum[1] / n), b = int(sum[2] / n);
        ZT_TRUE("средний цвет холодный, как у верного разбора (" + num(r) + "," + num(g) + "," +
                    num(b) + "), а не тёплый, как у ошибки Qt",
                b > r);
    }
}

void checkLimitsMatter(const QString& root) {
    const QString path = root + "/art/leonardo-oldmen.jpg";
    if (!QFileInfo::exists(path)) return;

    // Тот же файл при разных числах обязан идти РАЗНЫМИ путями — иначе
    // настройки ни на что не влияют, и проверка таблицы пуста.
    ImportLimits tiny;
    tiny.maxFileSizeMb = 0;   // бюджета файла нет вовсе
    const ImportResult r1 = importImage(path, tiny);
    ZT_EQ("при нулевом бюджете файла транскод не годится", std::string("путь фото"),
          std::string(routeName(r1.route)));

    ImportLimits roomy;
    roomy.maxFileSizeMb = 8;
    const ImportResult r2 = importImage(path, roomy);
    ZT_EQ("при щедром бюджете идёт транскод", std::string("транскод JPEG"),
          std::string(routeName(r2.route)));

    // И размер тоже должен слушаться.
    ImportLimits small;
    small.maxSize = 400;
    const ImportResult r3 = importImage(path, small);
    ZT_TRUE("при S=400 картинка уменьшена (" + num(r3.size.width) + ")", r3.size.width <= 1200);
}

void checkPixels() {
    ImportLimits limits;
    // Из буфера обмена: формата нет, решает только проба.
    QImage shot(600, 400, QImage::Format_RGBX8888);
    shot.fill(Qt::white);
    for (int y = 100; y < 300; ++y)
        for (int x = 50; x < 550; ++x)
            shot.setPixelColor(x, y, (x / 7 + y / 7) % 2 ? Qt::black : Qt::white);
    const ImportResult flat = importPixels(shot, limits);
    ZT_EQ("рисунок из буфера идёт в lossless", std::string("lossless"),
          std::string(routeName(flat.route)));

    // Шум сжимается плохо и обязан уйти на путь фото.
    QImage noise(600, 400, QImage::Format_RGBX8888);
    unsigned seed = 12345;
    for (int y = 0; y < noise.height(); ++y)
        for (int x = 0; x < noise.width(); ++x) {
            seed = seed * 1664525u + 1013904223u;
            noise.setPixelColor(x, y, QColor(int(seed >> 24), int((seed >> 16) & 0xFF),
                                             int((seed >> 8) & 0xFF)));
        }
    const ImportResult rough = importPixels(noise, limits);
    ZT_EQ("шум идёт путём фото", std::string("путь фото"),
          std::string(routeName(rough.route)));

    ZT_EQ("пустая картинка отвергается", std::string("отказ"),
          std::string(routeName(importPixels(QImage(), limits).route)));
}

void checkBombs(const QString& root) {
    ImportLimits limits;
    for (const char* bomb : {"bombs/бомба-миллиард-на-один.png", "bombs/бомба-сторона.png",
                             "bombs/бомба-площадь.png", "bombs/бомба-квадрат-предел.png",
                             "bombs/бомба-нулевая.png"}) {
        const QString path = root + "/" + bomb;
        if (!QFileInfo::exists(path)) continue;
        const ImportResult r = importImage(path, limits);
        ZT_EQ(std::string("бомба отвергнута: ") + bomb, std::string("отказ"),
              std::string(routeName(r.route)));
        ZT_TRUE(std::string("и объяснено: ") + bomb, !r.message.isEmpty());
    }
}

}  // namespace

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    QImageReader::setAllocationLimit(2048);
    checkPixels();
    if (argc > 1) {
        const QString root = QString::fromLocal8Bit(argv[1]);
        checkTable(root);
        checkTiffPath(root);
        checkLimitsMatter(root);
        checkBombs(root);
    }
    return zt::report("конвейер вставки");
}
