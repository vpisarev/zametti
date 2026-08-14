#include "import.h"

#include "image_read.h"

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

// ПОРОГИ. Их два, и оба отвечают на разные вопросы.
//
// Стоит ли вообще трогать чужой файл: берём свой вариант, только если он
// выигрывает пятую часть. Замер на ста самых лёгких Wallpapers — правило
// срабатывает у 37% файлов и даёт 35.1 МБ против 37.7 у сплошного транскода,
// никогда не будучи хуже; сплошное пережатие дало бы 41.2 МБ.
inline constexpr double kRecompressWin = 0.8;

// Фора точной версии: ей разрешено быть на 15% тяжелее, потому что взамен она
// даёт точные пиксели. Замер на 56 рисунках Леонардо и 24 файлах разных
// форматов: медиана отношения lossless/lossy 4.65 и 4.80 при минимуме 3.5 —
// то есть выигрывает точная версия только там, где картинка ДЕЙСТВИТЕЛЬНО
// плоская.
inline constexpr double kLosslessEdge = 1.15;

// Качество кандидата зависит от того, С ЧЕМ ОН СПОРИТ, а не от того, уменьшали
// ли картинку. Там, где кандидат должен выиграть у исходника пятую часть,
// проиграть спор из-за скупости на качество глупо; там, где спора нет (формат
// мы всё равно не храним), завышать качество незачем.
inline constexpr int kQualityVsSource = 95;

// Может ли картинка остаться в своём формате. У этих троих есть чем ответить:
// JXL и WebP уже сжаты хорошо, у JPEG есть байт-точный транскод. Остальные
// форматы мы не храним, и вопрос «оставить как есть» для них не стоит.
bool ownFormat(const QString& format) {
    return format == QLatin1String("jxl") || format == QLatin1String("webp") ||
           format == QLatin1String("jpeg") || format == QLatin1String("jpg");
}

// Кандидат: лучшее, во что мы можем превратить эту картинку, НЕ МЕНЯЯ размера.
//
// Кандидатов не больше двух — lossy и (если источник точен) lossless. Побеждает
// меньший, но у точного фора. Больше вариантов перебирать нечего: разрешение на
// этом шаге уже решено, а качество задано тем, с чем предстоит спорить.
struct Candidate {
    QByteArray bytes;
    bool lossless = false;
    int quality = 0;
    int encodes = 0;
};

Candidate bestCandidate(const QImage& image, const EncodeMeta& meta,
                        const ImportLimits& limits, bool exactAllowed, int quality) {
    Candidate out;
    QString err;

    EncodeOptions lossy;
    lossy.quality = quality;
    lossy.maxBitsPerChannel = limits.maxBitsPerChannel;
    out.bytes = encodeJxl(image, lossy, meta, &err);
    out.quality = quality;
    out.encodes = 1;

    // ТОЧНУЮ ВЕРСИЮ ПРЕДЛАГАЕМ ТОЛЬКО LOSSLESS-ИСТОЧНИКУ. У lossy точности уже
    // нет, и она станет честно кодировать в том числе артефакты чужого сжатия:
    // замер даёт 13 МБ против 0.9 у q95 на одном и том же файле.
    if (exactAllowed) {
        EncodeOptions exact;
        exact.lossless = true;
        exact.maxBitsPerChannel = limits.maxBitsPerChannel;
        const QByteArray precise = encodeJxl(image, exact, meta, &err);
        ++out.encodes;
        if (!precise.isEmpty() &&
            (out.bytes.isEmpty() ||
             double(precise.size()) <= double(out.bytes.size()) * kLosslessEdge)) {
            out.bytes = precise;
            out.lossless = true;
            out.quality = 0;
        }
    }
    return out;
}

// Точен ли источник. У JPEG ответ известен заранее, у WebP лежит в контейнере,
// у прочих форматов, которые к нам доходят, потерь не бывает по устройству.
bool sourceIsLossless(const QString& format, const QByteArray& raw) {
    if (format == QLatin1String("jpeg") || format == QLatin1String("jpg")) return false;
    if (format == QLatin1String("webp"))
        return webpIsLossless(std::string_view(raw.constData(), size_t(raw.size())));
    // JXL бывает и такой и такой, но спросить об этом дёшево нельзя — только
    // разжав. Считаем точным: ошибка в эту сторону стоит одного лишнего
    // энкода, а в обратную — потери точности у картинки, которая её имела.
    return true;
}

// Пиксели и метаданные источника. TIFF читаем своим читателем: Qt тихо
// перевирает CIELab, а метаданных не отдаёт вовсе.
struct Pixels {
    QImage image;
    EncodeMeta meta;
    bool ok = false;
    QString error;
};

Pixels readPixels(const QString& path, const QByteArray& raw, const SourceInfo& info,
                  const ImportLimits& limits) {
    Pixels out;
    if (info.format == QLatin1String("tiff") || info.format == QLatin1String("tif")) {
        TiffImage tiff;
        QString err;
        if (!readTiff(path, &tiff, &err, qint64(limits.decodeBudgetBytes()))) {
            out.error = QStringLiteral("TIFF не прочитан: %1").arg(err);
            return out;
        }
        out.image = tiff.image;
        out.meta.exif = tiff.exif;
        out.meta.xmp = tiff.xmp;
        if (!tiff.converted) out.meta.icc = tiff.icc;
    } else {
        DecodeRequest request;
        request.applyOrientation = true;   // поворот применяем к пикселям
        // ВВОЗУ ГЛУБИНА НУЖНА. Он уменьшает и пережимает, а округление до
        // байта по дороге теряется навсегда — ради этого jpegli и вендорен.
        // Замер на пятнадцати полотнах Эрмитажа (эталон — наш читатель TIFF,
        // кодировщик нейтральный, судья SSIMULACRA2): без уменьшения
        // шестнадцать бит дают +0.12 сверх восьми, С УМЕНЬШЕНИЕМ — +0.27, и
        // выигрывают 15 из 15.
        request.deep = true;
        out.image = decodeImage(raw, request);
        if (out.image.isNull()) {
            out.error = QStringLiteral("формат не поддерживается");
            return out;
        }
        out.meta = metaFor(raw);
    }

    // Имя исходника — в XMP. В хранилище файл зовётся бессмысленным id, и без
    // этого имя теряется, стоит вынести вложение наружу.
    const QByteArray name = QFileInfo(path).fileName().toUtf8();
    const std::string xmp = xmpWithFileName(
        std::string_view(out.meta.xmp.constData(), size_t(out.meta.xmp.size())),
        std::string_view(name.constData(), size_t(name.size())));
    out.meta.xmp = QByteArray(xmp.data(), qsizetype(xmp.size()));
    out.ok = true;
    return out;
}

// Кандидат не выиграл спор: своё оставляем при себе. У JPEG «своё» — это
// байт-точный транскод, он даёт то же самое и обычно меньше.
ImportResult keepOrTranscode(const QByteArray& raw, const SourceInfo& info) {
    ImportResult out;
    out.sourceBytes = raw.size();
    out.size = info.size;
    out.bitsPerSample = info.bitsPerSample;

    if (info.format == QLatin1String("jpeg") || info.format == QLatin1String("jpg")) {
        QString err;
        const QByteArray jxl = transcodeJpegToJxl(raw, &err);
        if (!jxl.isEmpty()) {
            // САМОПРОВЕРКА, инвариант A этапа 8: собираем исходный файл обратно
            // и сверяем БАЙТЫ. Это строже отпечатка и столько же стоит.
            const QByteArray back = reconstructJpeg(jxl, &err);
            if (back == raw && jxl.size() < raw.size()) {
                out.route = Route::TranscodedJpeg;
                out.bytes = jxl;
                out.extension = QStringLiteral("jxl");
                out.bitsPerSample = 8;
                return out;
            }
        }
        out.message = QStringLiteral("транскод не сошёлся, файл положен как есть");
        out.extension = QStringLiteral("jpg");
    } else {
        out.extension = info.format == QLatin1String("webp") ? QStringLiteral("webp")
                                                             : QStringLiteral("jxl");
        out.message = QStringLiteral("пережатие не дало выигрыша");
    }
    out.route = Route::AsIs;
    out.bytes = raw;
    return out;
}

// Путь фото: уменьшить Lanczos до бюджета и сжать. Один энкод.
//
// УМЕНЬШАЕМ LANCZOS, А НЕ УСРЕДНЕНИЕМ ПО ПЛОЩАДИ. Прежнее правило стоило
// четырёх-пяти баллов на каждой фотографии: на снимках DxO против оригинала
// area даёт 49.92 / 38.15 / 50.61, Lanczos — 54.38 / 42.98 / 56.21, ценой семи
// процентов байт. Ореолов, ради которых правило вводилось, глазами не видно ни
// на листве, ни на мелком тексте скриншота.
ImportResult photoPath(const QString& path, const QByteArray& raw, const SourceInfo& info,
                       const ImportLimits& limits) {
    Pixels pixels = readPixels(path, raw, info, limits);
    if (!pixels.ok) return refuse(Refusal::None, pixels.error);

    const bool colorFixed = canonicalizeColor(pixels.image, pixels.meta.icc);
    const Size target = targetSize({pixels.image.width(), pixels.image.height()}, limits);
    const QImage scaled =
        (target.width == pixels.image.width() && target.height == pixels.image.height())
            ? pixels.image
            : resampleLanczos(pixels.image, target.width, target.height);

    EncodeOptions opt;
    opt.quality = limits.quality;
    opt.maxBitsPerChannel = limits.maxBitsPerChannel;
    QString err;
    const QByteArray jxl = encodeJxl(scaled, opt, pixels.meta, &err);
    if (jxl.isEmpty())
        return refuse(Refusal::None, QStringLiteral("не удалось сжать: %1").arg(err));

    ImportResult out;
    out.route = Route::Photo;
    out.bytes = jxl;
    out.extension = QStringLiteral("jxl");
    out.size = {scaled.width(), scaled.height()};
    out.bitsPerSample = scaled.depth() > 32 ? std::min(16, limits.maxBitsPerChannel) : 8;
    out.quality = limits.quality;
    out.encodes = 1;
    out.sourceBytes = raw.size();
    if (colorFixed) out.message = QStringLiteral("цвет приведён к Display P3");
    return out;
}

}  // namespace

SourceInfo probeSource(const QString& path, const ImportLimits& limits) {
    SourceInfo info;
    const QFileInfo fi(path);
    info.fileBytes = fi.size();

    // Своя дверь: формат опознаётся по подписи, а не по имени файла, и
    // размеры берутся из шапки, без разжатия.
    const ImageProbe probe = probeImageFile(path);
    info.format = probe.format;
    info.animated = probe.frames > 1;
    info.size = {probe.size.width(), probe.size.height()};
    info.bitsPerSample = probe.bitsPerSample;
    info.hasAlpha = probe.hasAlpha;

    // Чужой формат наша дверь не опознала — спрашиваем Qt, он же его и прочтёт.
    // Тут он и остаётся полезен: широта ввоза дороже чистоты графа.
    if (info.format.isEmpty()) {
        QImageReader reader(path);
        info.format = QString::fromLatin1(reader.format()).toLower();
        info.animated = reader.imageCount() > 1 || reader.supportsAnimation();
        const QSize size = reader.size();
        info.size = {size.width(), size.height()};
        const QVariant fmt = reader.imageFormat();
        if (fmt.isValid()) {
            const auto f = static_cast<QImage::Format>(fmt.toInt());
            if (f == QImage::Format_RGBA64 || f == QImage::Format_RGBX64 ||
                f == QImage::Format_Grayscale16)
                info.bitsPerSample = 16;
            info.hasAlpha = QImage(1, 1, f).hasAlphaChannel();
        }
    }

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

    // --- РАЗВИЛКА 1: РАЗРЕШЕНИЕ ------------------------------------------
    //
    // Она первая, потому что единственная убирает развилки ЦЕЛИКОМ, а не
    // сужает их. У уменьшаемой картинки транскод невозможен (он сохраняет
    // размер), сравнивать с исходником бессмысленно (размеры разные), а точная
    // версия заведомо проиграет. Значит дальше обсуждать нечего: уменьшить и
    // сжать.
    const Size target = targetSize(info.size, limits);
    if (target != info.size) return photoPath(path, raw, info, limits);

    // Дальше картинка останется в своём размере, и вопрос только в том, чем её
    // закодировать.

    // --- РАЗВИЛКА 2: С ЧЕМ СПОРИМ ----------------------------------------
    //
    // Свой формат — значит у картинки есть чем ответить (JXL и WebP уже сжаты,
    // у JPEG есть транскод), и наш кандидат обязан выиграть пятую часть. Раз
    // спор, качество берём с запасом: проиграть его из-за скупости глупо.
    const bool own = ownFormat(info.format);
    const int quality = own ? kQualityVsSource : limits.quality;

    // JXL мы не разжимаем зря: если он уже нашего размера, шанс, что мы
    // пережмём его на пятую часть тем же кодеком, мал, а разжатие стоит
    // времени. Проверяем дёшево — по числу байт на пиксель.
    Pixels pixels = readPixels(path, raw, info, limits);
    if (!pixels.ok) return refuse(Refusal::None, pixels.error);

    const bool colorFixed = canonicalizeColor(pixels.image, pixels.meta.icc);

    // --- РАЗВИЛКА 3: ВИД СЖАТИЯ ИСТОЧНИКА --------------------------------
    const Candidate best =
        bestCandidate(pixels.image, pixels.meta, limits, sourceIsLossless(info.format, raw),
                      quality);
    if (best.bytes.isEmpty())
        return refuse(Refusal::None, QStringLiteral("не удалось сжать"));

    // --- ИСХОД -----------------------------------------------------------
    //
    // У своего формата кандидат должен выиграть пятую часть; не выиграл —
    // JPEG уходит транскодом, остальные остаются как есть. У чужого формата
    // спора нет: его мы всё равно не храним.
    if (own && double(best.bytes.size()) > double(raw.size()) * kRecompressWin)
        return keepOrTranscode(raw, info);

    ImportResult out;
    out.route = best.lossless ? Route::Lossless : Route::Photo;
    out.bytes = best.bytes;
    out.extension = QStringLiteral("jxl");
    out.size = info.size;
    out.bitsPerSample = pixels.image.depth() > 32 ? std::min(16, limits.maxBitsPerChannel) : 8;
    out.quality = best.quality;
    out.encodes = best.encodes;
    out.sourceBytes = raw.size();
    if (own)
        out.message = QStringLiteral("пережат: %1% от исходного")
                          .arg(100 * best.bytes.size() / std::max<qint64>(1, raw.size()));
    if (colorFixed) {
        const QString note = QStringLiteral("цвет приведён к Display P3");
        out.message = out.message.isEmpty() ? note : out.message + QStringLiteral("; ") + note;
    }
    return out;
}

ImportResult importPixels(const QImage& image, const ImportLimits& limits) {
    if (image.isNull()) return refuse(Refusal::Empty);
    const Refusal r = checkSource(image.width(), image.height(),
                                  image.depth() > 32 ? 16 : 8, limits);
    if (r != Refusal::None) return refuse(r);

    // Из буфера обмена формата нет, значит и спорить не с чем: качество
    // обычное, а точная версия участвует всегда — пиксели пришли точными.
    QImage pixels = image;
    const Size target = targetSize({image.width(), image.height()}, limits);
    if (target.width != image.width() || target.height != image.height())
        pixels = resampleLanczos(image, target.width, target.height);

    const Candidate best = bestCandidate(pixels, EncodeMeta{}, limits,
                                         target == Size{image.width(), image.height()},
                                         limits.quality);
    if (best.bytes.isEmpty()) return refuse(Refusal::None, QStringLiteral("не удалось сжать"));

    ImportResult out;
    out.route = best.lossless ? Route::Lossless : Route::Photo;
    out.bytes = best.bytes;
    out.extension = QStringLiteral("jxl");
    out.size = {pixels.width(), pixels.height()};
    out.bitsPerSample = pixels.depth() > 32 ? std::min(16, limits.maxBitsPerChannel) : 8;
    out.quality = best.quality;
    out.encodes = best.encodes;
    return out;
}

}  // namespace zametti
