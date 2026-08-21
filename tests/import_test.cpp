// Таблица решений конвейера: каждый ряд на настоящем файле.
//
// Проверяется не «сжалось», а КАКИМ ПУТЁМ пошло. «Сжалось» само по себе не
// значит ничего: картинка, которая должна была лечь байт в байт транскодом, а
// пошла путём фото, тоже «сжалась» — только потеряла при этом качество
// необратимо.

#include "image_read.h"
#include "import.h"

#include "test_util.h"

#include <vector>
#include "testdata.h"

#include <QBuffer>
#include <QColorSpace>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QImageReader>

#include <cstdio>
#include <string>

using namespace zametti;

namespace {

std::string num(long long v) { return std::to_string(v); }

QImage decodeResult(const ImportResult& r) {
    return decodeImage(r.bytes);
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
    const QSize src = probeImageFile(path).size;
    if (src.isValid() && r.size.width > 0) {
        ZT_TRUE(what + "не растянуто по ширине", r.size.width <= src.width());
        ZT_TRUE(what + "не растянуто по высоте", r.size.height <= src.height());
    }
    // ПЛОЩАДЬ НЕ БОЛЬШЕ БЮДЖЕТА — правило владельца, и оно НА ВСЕХ ПУТЯХ
    // сразу: «больше бюджета (S×S) у нас в принципе никогда ничего не
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
    ImportLimits limits;   // умолчания: S=2160 (бюджет 4.67 Мп), качество 90

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
    // А 4 Мп при нынешнем бюджете влезают целиком, и для них транскод — лучший
    // выбор: байт в байт дешевле, чем пережимать уже сжатое. (При S=1920 эта же
    // картинка уменьшалась и шла путём фото.)
    expectRoute(root, "photo/wallpaper-4mp.jpg", Route::TranscodedJpeg,
                "4 Мп влезают целиком — транскод", limits);

    // --- ряд «JXL» → как есть --------------------------------------------
    expectRoute(root, "formats/dice.jxl", Route::AsIs, "JXL влезает — кладём как есть", limits);

    // --- ряд «скриншот» → путь фото, и это ПОСЧИТАНО ---------------------
    //
    // Скриншот не влезает по пикселям (5.42 Мп при бюджете 4.67), но решает не
    // это: размер решается первым, кодек вторым, и уменьшенный скриншот честно
    // спорит обоими кандидатами. Он проигрывает — 297 КБ против 205 у lossy,
    // отношение 1.45 при форе точной версии 1.15.
    //
    // ЦЕНУ ТОЧНОСТИ ОТКЛОНИЛ ВЛАДЕЛЕЦ: 45% лишнего веса за буквы, которые в
    // lossy у JXL неотличимы при двукратном увеличении. Фора ненадолго
    // поднималась до 2.0 ради этого самого ряда — и была опущена обратно
    // словами «два раза это нереалистично много».
    //
    // Ряд менял ожидание трижды, и ни разу не от вкуса: сперва из-за ПРОБЫ на
    // уменьшенной копии (она давала 1.41 там, где правда 0.80), потом из-за
    // смены бюджета, теперь из-за цены точности. Числа рядом с каждым.
    expectRoute(root, "screen/screenshot-alpha.png", Route::Photo,
                "скриншот: точная версия дороже на 45%, и это посчитано", limits);

    // ФОРА ТОЧНОЙ ВЕРСИИ ДЕЙСТВУЕТ, А НЕ ЛЕЖИТ. Настройка приходит параметром —
    // и однажды уже приходила впустую: решение спрашивало вшитую константу, а
    // число из конфига доезжало до структуры и там умирало. Молчал об этом
    // ВЕСЬ набор: маршруты-то были верные — с константой.
    //
    // Поэтому порог проверяется не значением, а ДЕЙСТВИЕМ: тем же файлом,
    // двумя порогами по разные стороны от его отношения (1.45), и маршрут
    // обязан перевернуться. Умрёт параметр — оба ряда дадут одно и то же.
    {
        ImportLimits tight = limits;
        tight.losslessThreshold = 1.2;   // ниже отношения — точная не проходит
        expectRoute(root, "screen/screenshot-alpha.png", Route::Photo,
                    "порог ниже отношения — тот же скриншот идёт путём фото", tight);

        ImportLimits generous = limits;
        generous.losslessThreshold = 1.7;   // выше — проходит
        expectRoute(root, "screen/screenshot-alpha.png", Route::Lossless,
                    "порог выше отношения — он же остаётся точным", generous);
    }

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

    // --- ряды «крупнее бюджета» → путь фото, БЕЗ РАЗВИЛОК ------------------
    //
    // Первая развилка правил: не влезает — уменьшить и сжать, и обсуждать
    // больше нечего. Формат тут не имеет значения совсем, и это проверяется на
    // трёх разных форматах: свой JXL, свой WebP и чужой PNG. Все три обязаны
    // прийти к одному исходу.
    expectRoute(root, "formats/jxl-крупный.jxl", Route::Photo,
                "JXL крупнее бюджета уменьшается, как все", limits);
    expectRoute(root, "formats/webp-крупный.webp", Route::Photo,
                "WebP крупнее бюджета уменьшается, как все", limits);

    // --- ряд «JPEG, который стоит пережать» → q95 --------------------------
    //
    // Вторая половина правила про JPEG: если q95 выигрывает пятую часть, берём
    // его вместо байт-точного транскода. Замер на этом файле: 219 КБ исходник,
    // 106 КБ у q95, то есть 49%.
    {
        const QString loose = root + "/formats/jpeg-рыхлый.jpg";
        if (QFileInfo::exists(loose)) {
            const ImportResult r = importImage(loose, limits);
            ZT_EQ("рыхлый JPEG пережимается, а не транскодируется",
                  std::string("photo path"), std::string(routeName(r.route)));
            ZT_TRUE("и выигрыш действительно крупный (" +
                        num(100 * r.bytes.size() / QFileInfo(loose).size()) + "%)",
                    r.bytes.size() * 100 <= QFileInfo(loose).size() * 80);
        }
    }

    // --- ряды «чужие форматы» → кандидаты состязаются ---------------------
    //
    // HEIF и AVIF влезают по пикселям, источник у них точным не считается
    // только если это webp или jpeg, — значит состязаются оба кандидата. На
    // фотографии побеждает lossy, и это правильный исход; важно, что точная
    // версия ВООБЩЕ участвует (прежде эти форматы всегда шли lossy).
    // СОБРАНО С ПОДДЕРЖКОЙ — ЗНАЧИТ ПРОВЕРЯЕТСЯ (решение владельца): спрашиваем
    // ЯДРО (heifSupported), а не макрос сборки набора и не Qt. Макрос отвечал
    // за то, как собран НАБОР, и мог разойтись с тем, как собрано ядро; Qt же
    // про наш читатель не знает вовсе.
    std::printf("ядро %s avif и heic\n", zametti::heifSupported() ? "ЧИТАЕТ" : "не читает");
    if (zametti::heifSupported()) {
        expectRoute(root, "formats/heif-green-leaf.heif", Route::Photo,
                    "HEIF: кандидаты состязаются, побеждает lossy", limits);
        expectRoute(root, "formats/avif-sample.avif", Route::Photo,
                    "AVIF: то же самое", limits);
        // И ещё два ряда — те, на которых владелец и споткнулся: обычный avif с
        // телефона и heic. ПРОБА ОБЯЗАНА ЗНАТЬ РАЗМЕР: раньше его спрашивали у
        // Qt, который про avif не знает вовсе, — проба выходила пустой, и ввоз
        // отказывал «формат не поддержан» ещё до нашего декодера, даже в
        // сборке с WITH_HEIF=ON.
        for (const char* name : {"один-кадр/8бит-p3.avif", "один-кадр/10бит.heic"}) {
            const QString path = QDir(root).filePath(QString::fromUtf8(name));
            if (!QFileInfo::exists(path)) continue;   // корпуса может не быть
            const zametti::ImageProbe probe = zametti::probeImageFile(path);
            ZT_TRUE(std::string(name) + ": опознан как картинка", probe.valid());
            ZT_EQ(std::string(name) + ": формат heif", std::string("heif"),
                  probe.format.toStdString());
            ZT_TRUE(std::string(name) + ": размер прочитан до разжатия",
                    probe.size.width() > 0 && probe.size.height() > 0);
        }
        // И ШЛЮЗЫ ОКНА ЗНАЮТ ЭТИ ФОРМАТЫ: по этому списку строится фильтр
        // файлового диалога, а перетащенное отбирается пробой. Пока оба
        // спрашивали Qt, avif молча вставлялся ссылкой.
        const QStringList exts = zametti::readableImageExtensions();
        for (const char* ext : {"avif", "heic", "heif"})
            ZT_TRUE(std::string("расширение ") + ext + " в списке читаемых",
                    exts.contains(QString::fromLatin1(ext)));
    } else {
        // Не «файла нет», а «читателя нет»: молчаливый пропуск здесь означал бы,
        // что поломку читателя мы тоже не заметим.
        std::printf("ряды HEIF и AVIF пропущены: ядро собрано без WITH_HEIF\n");
    }

    // --- ряд «lossy-исходник не получает точную версию» -------------------
    //
    // САМЫЙ ТОНКИЙ РЯД таблицы, и без него правило проверялось бы только на
    // словах. Файл подобран так, что точная версия ВЫИГРАЛА БЫ, участвуй она:
    // замер даёт 2538 б у точной против 6228 у q95 при исходнике в 13404. Но
    // источник помечен lossy (чанк VP8 в контейнере), значит точная в
    // состязание не допускается вовсе — и исход обязан быть «пережат в q95»,
    // а не «lossless».
    //
    // Снимаю починку — разрешаю точную версию всем — и этот ряд краснеет
    // единственным во всём наборе.
    expectRoute(root, "formats/webp-lossy-плоский.webp", Route::Photo,
                "lossy-исходнику точная версия не предлагается, хотя выиграла бы", limits);

    // --- ряд «фотография в PNG» → путь фото ------------------------------
    expectRoute(root, "quality/png-16бит.png", Route::Photo,
                "фотография в PNG: проба уводит на путь фото", limits);

    // --- сырые форматы → честный отказ -----------------------------------
    for (const char* raw : {"raw/canon.cr3", "raw/olympus.orf", "raw/sony.arw",
                            "vivo/proraw.dng"}) {
        const QString path = root + "/" + raw;
        if (!QFileInfo::exists(path)) continue;
        const ImportResult r = importImage(path, limits);
        ZT_EQ(std::string("сырой формат отвергнут: ") + raw, std::string("refused"),
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
    ZT_EQ("Lab-TIFF идёт путём фото", std::string("photo path"),
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
    ZT_EQ("при обычных числах идёт транскод", std::string("JPEG transcode"),
          std::string(routeName(r2.route)));

    // Транскод возможен только пока картинка не уменьшается: он сохраняет
    // исходные коэффициенты, а с ними и размер. Ужать S — и путь обязан
    // смениться на путь фото.
    ImportLimits small;
    small.maxSize = 400;
    const ImportResult r3 = importImage(path, small);
    ZT_EQ("при тесном S транскод невозможен", std::string("photo path"),
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
    ZT_EQ("шум идёт путём фото", std::string("photo path"),
          std::string(routeName(rough.route)));

    ZT_EQ("пустая картинка отвергается", std::string("refused"),
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
        ZT_EQ(std::string("бомба отвергнута: ") + bomb, std::string("refused"),
              std::string(routeName(r.route)));
        ZT_TRUE(std::string("и объяснено: ") + bomb, !r.message.isEmpty());
    }
}

}  // namespace

static int ztRunSuite(int argc, char** argv) {
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

// Набор целиком одним TEST: тело не тронуто, argv ему собран здесь.
// Дробить на отдельные проверки — отдельная работа, по одному набору.
TEST(Import, All) {
    std::vector<QByteArray> ztArgs{QByteArrayLiteral("import_test")};
    ztArgs.push_back((zt::TestData::corpus(QStringLiteral("images/originals"))).toLocal8Bit());
    std::vector<char*> ztArgv;
    for (QByteArray& a : ztArgs) ztArgv.push_back(a.data());
    EXPECT_EQ(0, ztRunSuite(int(ztArgv.size()), ztArgv.data()));
}
