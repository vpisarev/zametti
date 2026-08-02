#include "import.h"

#include "color.h"
#include "exif.h"
#include "jxl_encoder.h"
#include "ladder.h"
#include "resample.h"
#include "tiff_reader.h"

#include <QColorSpace>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>

#include <algorithm>

namespace zametti {

const char* routeName(Route route) {
    switch (route) {
        case Route::Refused: return "отказ";
        case Route::AsIs: return "как есть";
        case Route::TranscodedJpeg: return "транскод JPEG";
        case Route::Lossless: return "lossless";
        case Route::Photo: return "путь фото";
    }
    return "?";
}

namespace {

QString refusalText(Refusal r) {
    switch (r) {
        case Refusal::None: return {};
        case Refusal::NoSize:
            return QStringLiteral("не удалось прочитать размеры — файл повреждён или это не "
                                  "картинка");
        case Refusal::Empty:
            return QStringLiteral("в заголовке нулевой размер");
        case Refusal::SideTooBig:
            return QStringLiteral("сторона больше 65535 пикселей");
        case Refusal::AreaTooBig:
            return QStringLiteral("площадь больше 512 мегапикселей");
        case Refusal::TooMuchMemory:
            return QStringLiteral("разжатая картинка не влезает в отведённую память");
    }
    return {};
}

ImportResult refuse(Refusal r, const QString& extra = {}) {
    ImportResult out;
    out.route = Route::Refused;
    out.refusal = r;
    out.message = extra.isEmpty() ? refusalText(r) : extra;
    return out;
}

// Метаданные для записи: EXIF со сброшенным поворотом (пиксели мы уже
// повернули), XMP как есть.
EncodeMeta metaFor(const QByteArray& file) {
    const ImageMeta src =
        readImageMeta(std::string_view(file.constData(), size_t(file.size())));
    EncodeMeta out;
    if (!src.exif.empty()) {
        std::string exif = src.exif;
        // Поворот СБРАСЫВАЕМ: читали мы с автоповоротом, значит в пикселях он
        // уже применён. Оставить тег значит повернуть картинку второй раз.
        resetExifOrientation(&exif);
        out.exif = QByteArray(exif.data(), qsizetype(exif.size()));
    }
    if (!src.xmp.empty()) out.xmp = QByteArray(src.xmp.data(), qsizetype(src.xmp.size()));
    return out;
}

// Сторона, до которой уменьшается копия для пробы. Она же — граница между
// «мелкой» и «крупной» картинкой: у мелкой копия совпадает с оригиналом, и
// проба перестаёт быть предсказанием, становясь точным ответом.
inline constexpr int kProbeSide = 800;

// ПЕРЕЖАТИЕ JPEG ВМЕСТО ТРАНСКОДА. Байт-точный транскод — хорошая вещь, но он
// сохраняет и то, что в исходнике сжато небрежно: JPEG, записанный с запасом,
// так и останется толстым. JXL при q95 нередко жмёт его заметно лучше.
//
// Правило владельца: берём пережатие, только если выигрыш КРУПНЫЙ — не меньше
// пятой части. Здесь есть асимметрия с порогом lossless (там lossless
// разрешено быть даже на 15% дороже), и она намеренная: там мы точность
// ПРИОБРЕТАЕМ, а здесь — теряем, причём пережимая уже сжатое. Значит и платить
// за это должно заметным выигрышем, и качество брать с запасом.
//
// Замер на сотне самых лёгких Wallpapers: правило срабатывает у 37% файлов и
// даёт 35.1 МБ против 37.7 у сплошного транскода. Важно, что оно НИКОГДА не
// хуже — где q95 проигрывает, остаётся транскод; сплошное пережатие всех
// подряд дало бы 41.2 МБ, то есть хуже нынешнего. На рисунках Леонардо (56
// сканов, плотная штриховка) правило не срабатывает ни разу — и правильно.
inline constexpr double kRecompressWin = 0.8;
// Качество пережатия. Выше рабочего: обратимость мы теряем, и запас качества —
// плата за это. Замер: средняя оценка 90.6 и 90.95 по шкале CID22, то есть
// «глазом не отличить».
inline constexpr int kRecompressQuality = 95;

// Проба на копии ~800 px: во сколько раз lossless тяжелее lossy. Малое
// отношение означает «плоскую» картинку — скриншот, схему, рисунок, — и ей
// место в lossless.
//
// Проба — ПРЕДСКАЗАНИЕ, и она обязана быть дешевле того, что предсказывает.
// Отсюда и восемьсот пикселей: полный энкод стоил бы столько же, сколько сам
// путь фото, и смысла в предсказании не осталось бы.
double losslessRatio(const QImage& image, const ImportLimits& limits) {
    const int side = 800;
    const double k = std::min(1.0, double(side) / std::max(image.width(), image.height()));
    const QImage small = k >= 1.0 ? image
                                  : resampleArea(image, std::max(1, int(image.width() * k)),
                                                 std::max(1, int(image.height() * k)));
    EncodeOptions lossy;
    lossy.quality = limits.quality;
    EncodeOptions lossless;
    lossless.lossless = true;

    QString err;
    const QByteArray a = encodeJxl(small, lossless, EncodeMeta{}, &err);
    const QByteArray b = encodeJxl(small, lossy, EncodeMeta{}, &err);
    if (a.isEmpty() || b.isEmpty()) return 1e9;   // не смогли предсказать — на путь фото
    return double(a.size()) / double(b.size());
}

// Путь фото: уменьшить до бюджета пикселей и сжать. Всё.
//
// Лестницы качества здесь больше нет — вместе с бюджетом файла (см. ladder.h).
// Платим за весь объём хранилища, а не за каждую картинку поодиночке: одни
// выходят мельче ожидаемого, другие крупнее. Замер на 321 файле шести
// каталогов: медиана 1.44 МБ, максимум 3.58, а потолок самого формата при q90
// — 8.2 МБ на шуме, которого в природе не бывает.
ImportResult photoPath(const QImage& image, const EncodeMeta& meta, const ImportLimits& limits) {
    const Size target = targetSize({image.width(), image.height()}, limits);
    // УМЕНЬШАЕМ LANCZOS, А НЕ УСРЕДНЕНИЕМ ПО ПЛОЩАДИ. Прежнее правило («в
    // сторону уменьшения только inter_area») стоило нам четырёх-пяти баллов на
    // каждой фотографии: замер на снимках DxO даёт против оригинала 49.92 /
    // 38.15 / 50.61 у area и 54.38 / 42.98 / 56.21 у Lanczos, ценой всего семи
    // процентов байт. Ореолов, ради которых правило вводилось, глазами не
    // видно ни на листве, ни на мелком тексте скриншота.
    const QImage scaled = (target.width == image.width() && target.height == image.height())
                              ? image
                              : resampleLanczos(image, target.width, target.height);

    EncodeOptions opt;
    opt.quality = limits.quality;
    opt.maxBitsPerChannel = limits.maxBitsPerChannel;
    QString err;
    const QByteArray jxl = encodeJxl(scaled, opt, meta, &err);
    if (jxl.isEmpty())
        return refuse(Refusal::None, QStringLiteral("не удалось сжать: %1").arg(err));

    ImportResult out;
    out.route = Route::Photo;
    out.bytes = jxl;
    out.extension = QStringLiteral("jxl");
    out.size = {scaled.width(), scaled.height()};
    out.bitsPerSample = scaled.depth() > 32 ? std::min(16, limits.maxBitsPerChannel) : 8;
    out.encodes = 1;
    out.quality = limits.quality;
    return out;
}

// Мелкая картинка: сжимаем ОБА варианта по-настоящему и берём тот, что меньше.
//
// Почему не по размеру и не по пробе. Замер на 56 рисунках Леонардо и 24
// файлах разных форматов: отношение lossless/lossy имеет медиану 4.65 и 4.80,
// и НИ ОДИН файл не прошёл бы в lossless ни при каком разумном пороге. Мелкая
// картинка, набитая деталями, ничем не отличается от большой: banner.jpg на
// 0.03 Мп стоит без потерь 31 КБ против 7 у lossy — вчетверо. Значит признак
// один — отношение, и размер о нём не говорит ничего.
//
// А вот СЧИТАТЬ это отношение для мелкой картинки надо точно: копия в 800 px
// совпадает с ней самой, и приближать нечего. Заодно и дешевле — победивший
// вариант уже сжат, повторно кодировать его не нужно.
ImportResult smallExact(const QImage& image, const EncodeMeta& meta,
                        const ImportLimits& limits, int* encodes) {
    EncodeOptions lossless;
    lossless.lossless = true;
    lossless.maxBitsPerChannel = limits.maxBitsPerChannel;
    EncodeOptions lossy;
    lossy.quality = limits.quality;

    QString err;
    const QByteArray a = encodeJxl(image, lossless, meta, &err);
    const QByteArray b = encodeJxl(image, lossy, meta, &err);
    if (encodes != nullptr) *encodes = 2;
    if (a.isEmpty()) return {};   // не вышло — пусть решает общий порядок

    // Порог тот же, что и у пробы: lossless разрешено быть чуть дороже, потому
    // что взамен он даёт точные пиксели.
    if (b.isEmpty() || double(a.size()) <= double(b.size()) * limits.losslessThreshold) {
        ImportResult out;
        out.route = Route::Lossless;
        out.bytes = a;
        out.extension = QStringLiteral("jxl");
        out.size = {image.width(), image.height()};
        out.bitsPerSample = image.depth() > 32 ? std::min(16, limits.maxBitsPerChannel) : 8;
        out.encodes = 2;
        return out;
    }
    return {};   // победил lossy — его считает путь фото, с лестницей
}

// Единственное место, где решается «плоская или фотография». Раньше это
// решение было выписано трижды — у webp, у png и у буфера обмена, — и всякая
// правка порядка требовала помнить про все три.
//
// Порядок такой:
//   1. мелочь жмём без потерь и смотрим на результат — проба для неё дороже
//      самого дела;
//   2. остальное решает проба на копии ~800 px.
ImportResult decideFlat(const QImage& image, const EncodeMeta& meta,
                        const ImportLimits& limits, int* encodes);

ImportResult losslessPath(const QImage& image, const EncodeMeta& meta,
                          const ImportLimits& limits) {
    // Уменьшать «плоские» картинки нельзя так же вольно, как фотографии: у
    // скриншота текст становится нечитаемым. Им разрешено быть вдвое крупнее
    // потолка стороны — до 2 × 3S.
    const int cap = 6 * limits.maxSize;
    QImage src = image;
    if (std::max(image.width(), image.height()) > cap) {
        const double k = double(cap) / std::max(image.width(), image.height());
        src = resampleArea(image, std::max(1, int(image.width() * k)),
                           std::max(1, int(image.height() * k)));
    }

    EncodeOptions opt;
    opt.lossless = true;
    opt.maxBitsPerChannel = limits.maxBitsPerChannel;

    QString err;
    const QByteArray jxl = encodeJxl(src, opt, meta, &err);
    if (jxl.isEmpty())
        return refuse(Refusal::None, QStringLiteral("не удалось сжать без потерь: %1").arg(err));

    ImportResult out;
    out.route = Route::Lossless;
    out.bytes = jxl;
    out.extension = QStringLiteral("jxl");
    out.size = {src.width(), src.height()};
    out.bitsPerSample = src.depth() > 32 ? std::min(16, limits.maxBitsPerChannel) : 8;
    out.encodes = 1;
    return out;
}

ImportResult decideFlat(const QImage& image, const EncodeMeta& meta,
                        const ImportLimits& limits, int* encodes) {
    if (encodes != nullptr) *encodes = 2;   // два энкода тратятся в любом случае

    // Мелкая — считаем точно; крупная — предсказываем по копии.
    if (std::max(image.width(), image.height()) <= kProbeSide) {
        int spent = 0;
        const ImportResult exact = smallExact(image, meta, limits, &spent);
        if (encodes != nullptr) *encodes = spent;
        if (exact.ok()) return exact;
        return photoPath(image, meta, limits);
    }

    const double ratio = losslessRatio(image, limits);
    return ratio <= limits.losslessThreshold ? losslessPath(image, meta, limits)
                                             : photoPath(image, meta, limits);
}

}  // namespace

SourceInfo probeSource(const QString& path, const ImportLimits& limits) {
    SourceInfo info;
    const QFileInfo fi(path);
    info.fileBytes = fi.size();

    QImageReader reader(path);
    reader.setAutoTransform(true);
    info.format = QString::fromLatin1(reader.format()).toLower();
    info.animated = reader.imageCount() > 1 || reader.supportsAnimation();

    // TIFF читаем своим читателем, но РАЗМЕРЫ и здесь берём у Qt: заголовок он
    // разбирает верно, врёт он только в цвете.
    const QSize size = reader.size();
    info.size = {size.width(), size.height()};

    // Глубину Qt по заголовку не сообщает; спрашиваем формат, который он
    // собирается отдать.
    const QVariant fmt = reader.imageFormat();
    info.bitsPerSample = 8;
    if (fmt.isValid()) {
        const auto f = static_cast<QImage::Format>(fmt.toInt());
        if (f == QImage::Format_RGBA64 || f == QImage::Format_RGBX64 ||
            f == QImage::Format_Grayscale16)
            info.bitsPerSample = 16;
    }
    info.hasAlpha = fmt.isValid() &&
                    QImage(1, 1, static_cast<QImage::Format>(fmt.toInt())).hasAlphaChannel();

    info.refusal = checkSource(info.size.width, info.size.height, info.bitsPerSample, limits);
    return info;
}

ImportResult importImage(const QString& path, const ImportLimits& limits) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return refuse(Refusal::NoSize, QStringLiteral("файл не открылся"));
    const QByteArray raw = f.readAll();
    f.close();

    const SourceInfo info = probeSource(path, limits);
    if (info.refusal != Refusal::None) return refuse(info.refusal);

    ImportResult result;
    result.sourceBytes = raw.size();
    const Size target = targetSize(info.size, limits);
    const bool fitsPixels = target == info.size;

    // --- JXL: уже наш формат ---------------------------------------------
    //
    // Для СВОЕГО формата решает БЮДЖЕТ ФАЙЛА, а не бюджет пикселей. Иначе
    // получается противоречие, на котором я и наступил: путь lossless нарочно
    // разрешает плоским картинкам быть крупнее S (у ужатого скриншота текст
    // нечитаем), а правило «JXL как есть» требовало влезать в тот же бюджет
    // пикселей. Из-за этого скриншот, положенный без потерь на полном размере,
    // при ПОВТОРНОМ прогоне recompress уезжал на путь фото и терял пиксели —
    // то есть ломался инвариант идемпотентности.
    //
    // Решает ПОТОЛОК ПИКСЕЛЕЙ, и тот же, что у lossless: шесть S. Он защищает
    // от чужого JXL в двадцать мегапикселей. Бюджета файла больше нет вовсе —
    // а раз так, чужой JXL разумного размера мы просто не трогаем: пережимать
    // уже сжатое тем же кодеком незачем.
    const int jxlCap = 6 * limits.maxSize;
    const bool fitsJxlCap = std::max(info.size.width, info.size.height) <= jxlCap;
    if (info.format == QLatin1String("jxl") && fitsJxlCap) {
        result.route = Route::AsIs;
        result.bytes = raw;
        result.extension = QStringLiteral("jxl");
        result.size = info.size;
        result.bitsPerSample = info.bitsPerSample;
        return result;
    }

    // --- JPEG: пережать, если сильно выигрываем; иначе транскод -----------
    //
    // Транскод возможен ТОЛЬКО когда картинка не уменьшается: он сохраняет
    // исходные коэффициенты, а с ними и размер. Не влезает по пикселям — идёт
    // обычным путём фото, ниже.
    if ((info.format == QLatin1String("jpeg") || info.format == QLatin1String("jpg")) &&
        fitsPixels) {
        // Проба пережатия. Разжать приходится в любом случае, если хотим
        // сравнить, — но энкод при q95 стоит недорого, а выигрыш бывает
        // двукратным.
        {
            QImageReader reader(path);
            reader.setAutoTransform(true);
            QImage pixels = reader.read();
            if (!pixels.isNull()) {
                EncodeMeta meta = metaFor(raw);
                // Цвет приводим и здесь: пережатый файл ложится в хранилище
                // насовсем, и правило «в хранилище только то, что переживёт
                // круг» на него распространяется так же (см. color.h).
                canonicalizeColor(pixels, meta.icc);
                EncodeOptions opt;
                opt.quality = kRecompressQuality;
                opt.maxBitsPerChannel = limits.maxBitsPerChannel;
                QString e;
                const QByteArray tighter = encodeJxl(pixels, opt, meta, &e);
                if (!tighter.isEmpty() &&
                    double(tighter.size()) <= double(raw.size()) * kRecompressWin) {
                    result.route = Route::Photo;
                    result.bytes = tighter;
                    result.extension = QStringLiteral("jxl");
                    result.size = info.size;
                    result.bitsPerSample = 8;
                    result.quality = kRecompressQuality;
                    result.encodes = 1;
                    result.message =
                        QStringLiteral("пережат вместо транскода: %1% от исходного")
                            .arg(100 * tighter.size() / std::max<qint64>(1, raw.size()));
                    return result;
                }
            }
        }
        QString err;
        const QByteArray jxl = transcodeJpegToJxl(raw, &err);
        if (!jxl.isEmpty()) {
            // САМОПРОВЕРКА, инвариант A этапа 8. Сравниваем сами байты — это
            // строже отпечатка и столько же стоит.
            const QByteArray back = reconstructJpeg(jxl, &err);
            if (back == raw && jxl.size() < raw.size()) {
                result.route = Route::TranscodedJpeg;
                result.bytes = jxl;
                result.extension = QStringLiteral("jxl");
                result.size = info.size;
                result.bitsPerSample = 8;
                return result;
            }
        }
        // Не сошлось — кладём исходник как есть. Терять байты из-за того, что
        // транскод не удался, нельзя.
        result.route = Route::AsIs;
        result.bytes = raw;
        result.extension = QStringLiteral("jpg");
        result.size = info.size;
        result.bitsPerSample = 8;
        result.message = QStringLiteral("транскод не сошёлся, файл положен как есть");
        return result;
    }

    // --- дальше нужны пиксели --------------------------------------------
    QImage image;
    EncodeMeta meta;
    if (info.format == QLatin1String("tiff") || info.format == QLatin1String("tif")) {
        // Свой читатель: Qt тихо перевирает CIELab, а метаданных не отдаёт
        // вовсе.
        TiffImage tiff;
        QString err;
        if (!readTiff(path, &tiff, &err, qint64(limits.decodeBudgetBytes())))
            return refuse(Refusal::None, QStringLiteral("TIFF не прочитан: %1").arg(err));
        image = tiff.image;
        meta.exif = tiff.exif;
        meta.xmp = tiff.xmp;
        if (!tiff.converted) meta.icc = tiff.icc;
    } else {
        QImageReader reader(path);
        reader.setAutoTransform(true);   // поворот применяем к пикселям
        image = reader.read();
        if (image.isNull())
            return refuse(Refusal::None,
                          QStringLiteral("формат не поддерживается: %1")
                              .arg(reader.errorString()));
        meta = metaFor(raw);
    }

    // --- имя исходника в XMP ----------------------------------------------
    //
    // В хранилище файл называется бессмысленным id, и человеческое имя живёт
    // только в заметке — как alt у картинки. Стоит вынести вложение наружу, и
    // имя потеряно. Кладём его внутрь самой картинки, в xmpMM:PreservedFileName.
    {
        const QByteArray name = QFileInfo(path).fileName().toUtf8();
        const std::string xmp = xmpWithFileName(
            std::string_view(meta.xmp.constData(), size_t(meta.xmp.size())),
            std::string_view(name.constData(), size_t(name.size())));
        meta.xmp = QByteArray(xmp.data(), qsizetype(xmp.size()));
    }

    // --- цвет: привести к тому, что переживёт круг ------------------------
    //
    // Здесь, а не в путях записи: приведение обязано случиться ДО любых проб и
    // решений, иначе проба lossless мерила бы одну картинку, а записывалась бы
    // другая. Молчаливо ничего не теряется — профили, выражаемые описанием
    // (sRGB, Display P3, Adobe RGB), проходят мимо нетронутыми.
    const bool colorFixed = canonicalizeColor(image, meta.icc);
    // Приписка идёт ко ВСЕМ дальнейшим исходам, а исходов ниже несколько, и
    // каждый собирает свой ImportResult заново. Отсюда обёртка, а не поле.
    const auto noteColor = [colorFixed](ImportResult r) {
        if (!colorFixed) return r;
        const QString note = QStringLiteral("цвет приведён к Display P3");
        r.message = r.message.isEmpty() ? note : r.message + QStringLiteral("; ") + note;
        return r;
    };

    // --- WebP, не уменьшаемый: пробуем перекодировать ---------------------
    //
    // Условие только по пикселям: бюджета файла больше нет. Уменьшаемый webp
    // и так пойдёт общим путём ниже — сравнивать его с исходником незачем,
    // размеры разные.
    if (info.format == QLatin1String("webp") && fitsPixels) {
        int spent = 0;
        ImportResult tried = decideFlat(image, meta, limits, &spent);
        tried.sourceBytes = raw.size();
        tried.encodes += spent;   // проба тоже энкоды
        // Заменяем только при заметном выигрыше: гонять байты ради пяти
        // процентов незачем, а вот потерять качество — запросто.
        if (tried.ok() && tried.bytes.size() * 100 <= raw.size() * 85) return noteColor(tried);
        result.route = Route::AsIs;
        result.bytes = raw;
        result.extension = QStringLiteral("webp");
        result.size = info.size;
        result.bitsPerSample = info.bitsPerSample;
        result.message = QStringLiteral("перекодирование не дало выигрыша");
        return result;
    }

    // --- PNG и прочее плоское: решает проба -------------------------------
    const bool flatCandidate = info.format == QLatin1String("png") ||
                               info.format == QLatin1String("gif") ||
                               info.format == QLatin1String("bmp");
    if (flatCandidate) {
        int spent = 0;
        ImportResult out = decideFlat(image, meta, limits, &spent);
        out.sourceBytes = raw.size();
        out.encodes += spent;
        return noteColor(out);
    }

    // --- всё прочее: путь фото -------------------------------------------
    ImportResult out = photoPath(image, meta, limits);
    out.sourceBytes = raw.size();
    return noteColor(out);
}

ImportResult importPixels(const QImage& image, const ImportLimits& limits) {
    if (image.isNull()) return refuse(Refusal::Empty);
    const Refusal r = checkSource(image.width(), image.height(),
                                  image.depth() > 32 ? 16 : 8, limits);
    if (r != Refusal::None) return refuse(r);

    // Из буфера обмена приходят и скриншоты, и фотографии, и рисунки — формата
    // у них нет, значит решает то же общее правило.
    int spent = 0;
    ImportResult out = decideFlat(image, EncodeMeta{}, limits, &spent);
    out.encodes += spent;
    return out;
}

}  // namespace zametti
