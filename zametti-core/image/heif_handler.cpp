#include "heif_handler.h"

#include <libheif/heif.h>

#include <QColorSpace>
#include <QImage>
#include <QIODevice>
#include <QVariant>

#include <cstring>

namespace zametti {

namespace {

// Подпись ISOBMFF: размер бокса (4 байта), затем 'ftyp', затем марка.
// Проверяем именно марку, а не только 'ftyp': тот же контейнер носит и видео,
// и перехватывать mp4 нам совершенно незачем.
constexpr int kSignatureBytes = 32;

// nclx → QColorSpace. Перечисляем руками ровно те сочетания, которые умеем
// назвать честно; всё прочее оставляем без профиля — соврать про цвет хуже,
// чем промолчать.
QColorSpace fromNclx(int primaries, int transfer) {
    const bool srgbCurve = transfer == heif_transfer_characteristic_IEC_61966_2_1 ||
                           transfer == heif_transfer_characteristic_ITU_R_BT_709_5 ||
                           transfer == heif_transfer_characteristic_unspecified;
    switch (primaries) {
        case heif_color_primaries_SMPTE_EG_432_1:   // Display P3
        case heif_color_primaries_SMPTE_RP_431_2:
            if (srgbCurve) return QColorSpace(QColorSpace::DisplayP3);
            break;
        case heif_color_primaries_ITU_R_BT_2020_2_and_2100_0:
            if (srgbCurve) return QColorSpace(QColorSpace::Bt2020);
            break;
        case heif_color_primaries_ITU_R_BT_709_5:
        case heif_color_primaries_unspecified:
            if (srgbCurve) return QColorSpace(QColorSpace::SRgb);
            break;
        default:
            break;
    }
    return {};
}

bool looksLikeHeif(const QByteArray& head) {
    if (head.size() < 12) return false;
    if (std::memcmp(head.constData() + 4, "ftyp", 4) != 0) return false;
    const heif_filetype_result r = heif_check_filetype(
        reinterpret_cast<const uint8_t*>(head.constData()), int(head.size()));
    return r == heif_filetype_yes_supported || r == heif_filetype_maybe;
}

}  // namespace

HeifHandler::HeifHandler() = default;

HeifHandler::~HeifHandler() {
    if (ctx_) heif_context_free(ctx_);
}

bool HeifHandler::peek(QIODevice* device) {
    if (!device) return false;
    return looksLikeHeif(device->peek(kSignatureBytes));
}

bool HeifHandler::canRead() const {
    if (headerFailed_ || scanned_) return false;
    return peek(device());
}

bool HeifHandler::readHeader() const {
    if (haveHeader_) return true;
    if (headerFailed_) return false;
    QIODevice* dev = device();
    if (!dev) { headerFailed_ = true; return false; }

    if (data_.isEmpty()) data_ = dev->readAll();
    if (data_.isEmpty()) { headerFailed_ = true; return false; }

    ctx_ = heif_context_alloc();
    if (!ctx_) { headerFailed_ = true; return false; }
    // ...without_copy: буфер data_ живёт всё время жизни обработчика, копия
    // многомегабайтного файла тут ни к чему.
    heif_error err = heif_context_read_from_memory_without_copy(
        ctx_, data_.constData(), size_t(data_.size()), nullptr);
    if (err.code != heif_error_Ok) { headerFailed_ = true; return false; }

    heif_image_handle* handle = nullptr;
    err = heif_context_get_primary_image_handle(ctx_, &handle);
    if (err.code != heif_error_Ok || !handle) { headerFailed_ = true; return false; }

    // ВАЖНО: эти размеры уже ПОВЁРНУТЫЕ. libheif применяет преобразования
    // контейнера сама (heif_decoding_options::ignore_transformations = 0 по
    // умолчанию). Проверено на файле с Orientation = 6: шапка обещает
    // 4032x3024, libheif отдаёт 3024x4032. Поэтому поворот мы Qt НЕ сообщаем —
    // иначе он повернул бы второй раз.
    size_ = QSize(heif_image_handle_get_width(handle), heif_image_handle_get_height(handle));
    hasAlpha_ = heif_image_handle_has_alpha_channel(handle) != 0;
    bitsPerPixel_ = heif_image_handle_get_luma_bits_per_pixel(handle);
    if (bitsPerPixel_ <= 0) bitsPerPixel_ = 8;

    // Цвет в HEIF описывается ДВУМЯ разными способами, и читать надо оба.
    // Первый — вложенный ICC-профиль; так делает iPhone. Второй — nclx: не
    // профиль, а ссылка на стандартные первичные цвета и кривую; так делают
    // почти все AVIF. Пока я читал только первый, оба avif корпуса приезжали
    // вовсе без профиля, то есть широкий охват молча превращался в sRGB — при
    // том что один из них ровно Display P3.
    const size_t iccSize = heif_image_handle_get_raw_color_profile_size(handle);
    if (iccSize > 0) {
        QByteArray icc(qsizetype(iccSize), Qt::Uninitialized);
        if (heif_image_handle_get_raw_color_profile(handle, icc.data()).code == heif_error_Ok)
            icc_ = icc;
    }
    if (icc_.isEmpty()) {
        heif_color_profile_nclx* nclx = nullptr;
        if (heif_image_handle_get_nclx_color_profile(handle, &nclx).code == heif_error_Ok && nclx) {
            named_ = fromNclx(nclx->color_primaries, nclx->transfer_characteristics);
            heif_nclx_color_profile_free(nclx);
        }
    }

    heif_image_handle_release(handle);
    haveHeader_ = true;
    return true;
}

bool HeifHandler::read(QImage* image) {
    if (!image || scanned_) return false;
    if (!readHeader()) return false;
    if (size_.isEmpty()) return false;

    heif_image_handle* handle = nullptr;
    if (heif_context_get_primary_image_handle(ctx_, &handle).code != heif_error_Ok || !handle)
        return false;

    // Глубже восьми бит — просим шестнадцатибитные отсчёты. В корпусе
    // владельца есть и 10-, и 12-битные avif; ужимать их до восьми нельзя.
    const bool deep = bitsPerPixel_ > 8;
    const heif_chroma chroma = deep ? heif_chroma_interleaved_RRGGBBAA_LE
                                    : heif_chroma_interleaved_RGBA;

    heif_image* img = nullptr;
    const heif_error err = heif_decode_image(handle, &img, heif_colorspace_RGB, chroma, nullptr);
    heif_image_handle_release(handle);
    if (err.code != heif_error_Ok || !img) return false;

    const int w = heif_image_get_width(img, heif_channel_interleaved);
    const int h = heif_image_get_height(img, heif_channel_interleaved);
    int stride = 0;
    const uint8_t* src = heif_image_get_plane_readonly(img, heif_channel_interleaved, &stride);
    if (!src || w <= 0 || h <= 0) { heif_image_release(img); return false; }

    QImage out(QSize(w, h), deep ? QImage::Format_RGBA64 : QImage::Format_RGBA8888);
    if (out.isNull()) { heif_image_release(img); return false; }

    if (deep) {
        // Отсчёты лежат в младших разрядах шестнадцатибитного слова: у
        // десятибитной картинки это 0..1023. Qt ждёт полный размах 0..65535,
        // так что растягиваем — иначе картинка выйдет вчетверо темнее.
        const int range = heif_image_get_bits_per_pixel_range(img, heif_channel_interleaved);
        const int bits = range > 0 ? range : bitsPerPixel_;
        const uint32_t maxIn = (1u << bits) - 1u;
        for (int y = 0; y < h; ++y) {
            const auto* s = reinterpret_cast<const uint16_t*>(src + size_t(y) * size_t(stride));
            auto* d = reinterpret_cast<uint16_t*>(out.scanLine(y));
            for (int i = 0; i < w * 4; ++i)
                d[i] = uint16_t((uint32_t(s[i]) * 65535u + maxIn / 2) / maxIn);
        }
    } else {
        const size_t rowBytes = size_t(w) * 4u;
        for (int y = 0; y < h; ++y)
            std::memcpy(out.scanLine(y), src + size_t(y) * size_t(stride), rowBytes);
    }
    heif_image_release(img);

    if (!hasAlpha_) {
        // Альфы нет — но буфер у нас четырёхканальный. Формат без альфы
        // честнее: иначе всё, что дальше, будет считать картинку прозрачной.
        out = out.convertToFormat(deep ? QImage::Format_RGBX64 : QImage::Format_RGBX8888);
    }

    if (!icc_.isEmpty()) {
        const QColorSpace cs = QColorSpace::fromIccProfile(icc_);
        if (cs.isValid()) out.setColorSpace(cs);
    } else if (named_.isValid()) {
        out.setColorSpace(named_);
    }

    *image = out;
    scanned_ = true;
    return true;
}

QVariant HeifHandler::option(ImageOption option) const {
    switch (option) {
        case Size:
            return readHeader() ? QVariant(size_) : QVariant();
        case ImageFormat:
            if (!readHeader()) return QVariant();
            return QVariant::fromValue(
                bitsPerPixel_ > 8 ? (hasAlpha_ ? QImage::Format_RGBA64 : QImage::Format_RGBX64)
                                  : (hasAlpha_ ? QImage::Format_RGBA8888
                                               : QImage::Format_RGBX8888));
        default:
            return QVariant();
    }
}

void HeifHandler::setOption(ImageOption, const QVariant&) {}

bool HeifHandler::supportsOption(ImageOption option) const {
    // ImageTransformation здесь намеренно НЕ поддерживается: поворот уже
    // применён внутри libheif, см. комментарий в readHeader().
    return option == Size || option == ImageFormat;
}

}  // namespace zametti
