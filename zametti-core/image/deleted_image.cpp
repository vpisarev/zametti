#include "deleted_image.h"

#include "exif.h"
#include "image_read.h"
#include "jxl_encoder.h"
#include "resample.h"

namespace zametti {

QByteArray makeDeletedImage(const QByteArray& file, const ImportLimits& limits, QString* error) {
    // Метаданные читаем ДО разжатия: у испорченного файла заголовок может
    // оказаться цел, и метку тогда всё равно есть на что положить.
    const ImageMeta meta =
        readImageMeta(std::string_view(file.constData(), size_t(file.size())));

    // Просим декодер отдать поменьше — это подсказка, а не уменьшение;
    // точную подгонку делаем сами лестницей ввоза.
    DecodeRequest request;
    request.maxSize = QSize(limits.maxSize * 3, limits.maxSize * 3);
    request.deep = false;
    request.applyOrientation = false;
    const QImage source = decodeImage(file, request);
    if (source.isNull()) {
        if (error != nullptr) *error = QStringLiteral("cannot decode the attachment");
        return {};
    }

    const Size want = targetSize(Size{source.width(), source.height()}, limits);
    // УМЕНЬШЕНИЕ — ТОЛЬКО LANCZOS (решение владельца): усреднение по площади
    // показало себя хуже, и путь ввоза давно уменьшает так же.
    const QImage small = want.width == source.width() && want.height == source.height()
                             ? source
                             : resampleLanczos(source, want.width, want.height);
    if (small.isNull()) {
        if (error != nullptr) *error = QStringLiteral("cannot downscale the attachment");
        return {};
    }

    EncodeOptions options;
    options.quality = limits.quality;
    options.lossless = false;
    options.maxBitsPerChannel = 8;   // призраку глубина ни к чему
    options.effort = 5;

    EncodeMeta out;
    out.exif = QByteArray(meta.exif.data(), qsizetype(meta.exif.size()));
    out.icc = QByteArray(meta.icc.data(), qsizetype(meta.icc.size()));
    // РЕВИЗИЯ РАСТЁТ, ПОМЕТКА СТАВИТСЯ. Ревизии не было вовсе (вложение ввезено
    // до этапа 17) — считаем её единицей: файл на диске и есть первая версия.
    const int rev = qMax(1, xmpZamettiRev(meta.xmp)) + 1;
    const std::string marked = xmpWithZamettiState(meta.xmp, rev, true);
    out.xmp = QByteArray(marked.data(), qsizetype(marked.size()));

    return encodeJxl(small, options, out, error);
}

}  // namespace zametti
