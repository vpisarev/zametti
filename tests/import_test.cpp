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
    // ПЛОЩАДЬ НЕ БОЛЬШЕ БЮДЖЕТА — правило владельца, и оно НА ВСЕХ ПУТЯХ
    // сразу: «больше 8 Мп (2880×2880) у нас в принципе никогда ничего не
    // добавляется». Проверяется на каждом файле, а не отдельным случаем,
    // потому что нарушить его может любая ветка — в том числе транскод,
    // который размер не меняет вовсе.
    if (r.size.width > 0) {
        const double budget = double(limits.maxSize) * limits.maxSize;
        const double area = double(r.size.width) * r.size.height;
        ZT_TRUE(what + "площадь в бюджете (" + num(qRound64(area / 1e6)) + " Мп из " +
                    num(qRound64(budget / 1e6)) + ")",
                area <= budget * 1.001);
    }
}

void checkTable(const QString& root) {
    ImportLimits limits;   // умолчания: S=2880 (бюджет 8.3 Мп), качество 90

    // --- ряд «JPEG влезает в оба бюджета» → байт-точный транскод ---------
    expectRoute(root, "art/leonardo-oldmen.jpg", Route::TranscodedJpeg,
                "небольшой JPEG влезает в оба бюджета", limits);
    expectRoute(root, "art/leonardo-tiny.jpg", Route::TranscodedJpeg, "крошечный JPEG", limits);

    // --- тот же ряд, но картинка не влезает по пикселям → путь фото -------
    //
    // Транскод сохраняет исходные коэффициенты, а с ними и размер: он возможен
    // ТОЛЬКО пока картинка не уменьшается. 60 Мп при бюджете в 8.3 уменьшаются
    // обязательно — значит транскод отпадает.
    expectRoute(root, "photo/sony-60mp.jpg", Route::Photo,
                "60 Мп уменьшается, транскод невозможен", limits);
    // А 4 Мп при S=2880 влезают целиком, и для них транскод — лучший выбор:
    // байт в байт дешевле, чем пережимать уже сжатое. (При прежнем S=1920 эта
    // же картинка уменьшалась и шла путём фото.)
    expectRoute(root, "photo/wallpaper-4mp.jpg", Route::TranscodedJpeg,
                "4 Мп влезают целиком — транскод", limits);

    // --- ряд «JXL» → как есть --------------------------------------------
    expectRoute(root, "formats/dice.jxl", Route::AsIs, "JXL влезает — кладём как есть", limits);

    // --- ряд «скриншот целого экрана» → путь фото ------------------------
    //
    // Раньше здесь ожидался lossless, и это было следствие щедрого порога
    // (lossless разрешалось быть втрое тяжелее lossy). Теперь порог 1.15, и
    // рассуждение владельца такое: скриншот ЦЕЛОГО экрана в заметку попадает
    // редко, он всё равно уменьшается до бюджета, буквы всё равно чуть плывут
    // от самого уменьшения, а художественной ценности в нём нет. Читаются
    // буквы — и довольно.
    //
    // Точность важна для ДРУГОГО случая — вырезки с экрана: ссылка, формула из
    // статьи, снимок товара. Такая вырезка почти всегда мельче 800 пикселей, и
    // для неё сравнение делается точное, без всякого предсказания (см. ряд
    // ниже).
    expectRoute(root, "screen/screenshot-alpha.png", Route::Photo,
                "скриншот целого экрана: lossless дороже, идём путём фото", limits);

    // --- ряд «мелкая вырезка» → решает точное сравнение -------------------
    //
    // Здесь предсказания нет вовсе: оба варианта сжимаются по-настоящему, и
    // побеждает тот, что меньше. Мультяшная графика с плоскими заливками —
    // случай, где lossless выигрывает честно (замер: 2 КБ против 4 у lossy).
    //
    // Ряд НА PNG, а не на webp: у webp с недавних пор своё правило, общее с
    // JPEG (пережатие q95 при выигрыше в пятую часть), и через сравнение
    // lossless/lossy он больше не проходит.
    {
        const QString flat = root + "/quality/плоская-графика.png";
        if (QFileInfo::exists(flat))
            expectRoute(root, "quality/плоская-графика.png", Route::Lossless,
                        "плоская графика: lossless дешевле, и это измерено", limits);
    }

    // --- ряды «webp» → правило то же, что у JPEG --------------------------
    //
    // Но с одной развилкой: у LOSSLESS-исходника сражаются обе версии JXL, у
    // lossy — только lossy. Вид сжатия спрашивается у контейнера (чанк VP8L
    // против VP8), а не у расширения: «.webp» о нём не говорит ничего.
    //
    // Один и тот же рисунок, сохранённый двумя способами, обязан пойти РАЗНЫМИ
    // путями — иначе различение не работает.
    // Фотографический lossless-webp: сражаются обе версии, побеждает q95 —
    // замер даёт 451 КБ против 2044 у точной при исходнике в 2067. То есть
    // выигрыш вчетверо, и он берётся.
    expectRoute(root, "formats/webp-lossless-фото.webp", Route::Photo,
                "lossless-webp с фотографией: q95 выигрывает вчетверо", limits);

    // А на плоской графике побеждает уже ТОЧНАЯ версия (4239 против 26315 у
    // q95) — но до порога в пятую часть не дотягивает: webp lossless на такой
    // картинке и сам хорош, наш выигрыш всего 5%. Значит оставляем как есть, и
    // это правильный исход: пережимать ради пяти процентов незачем.
    expectRoute(root, "formats/плоская-графика-lossless.webp", Route::AsIs,
                "lossless-webp с графикой: точная версия победила, но выигрыш мал", limits);

    // Тот же рисунок, сохранённый с потерями: точная версия в состязании не
    // участвует вовсе (исходнику её уже нечего дать), а q95 даёт 23577 при
    // исходнике 9556 — то есть вдвое хуже.
    expectRoute(root, "formats/плоская-графика-lossy.webp", Route::AsIs,
                "lossy-webp: пережатие не выиграло, оставляем как есть", limits);

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
    //
    // Бюджета файла больше нет вовсе (его убрал владелец: платим за весь объём
    // хранилища, а не за каждую картинку, и потолок у формата свой — 8.2 МБ на
    // шуме при q90). Значит различать пути должен ОСТАВШИЙСЯ рычаг — S.
    ImportLimits roomy;
    const ImportResult r2 = importImage(path, roomy);
    ZT_EQ("при обычных числах идёт транскод", std::string("транскод JPEG"),
          std::string(routeName(r2.route)));

    // Транскод возможен только пока картинка не уменьшается: он сохраняет
    // исходные коэффициенты, а с ними и размер. Ужать S — и путь обязан
    // смениться на путь фото.
    ImportLimits small;
    small.maxSize = 400;
    const ImportResult r3 = importImage(path, small);
    ZT_EQ("при тесном S транскод невозможен", std::string("путь фото"),
          std::string(routeName(r3.route)));
    ZT_TRUE("и картинка уменьшена (" + num(r3.size.width) + ")", r3.size.width <= 1200);
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
