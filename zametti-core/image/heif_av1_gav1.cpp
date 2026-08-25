#include "heif_av1_gav1.h"

#include <libheif/heif.h>
#include <libheif/heif_plugin.h>

#include <gav1/decoder.h>

#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace zametti {
namespace {

// Приоритет. Единственный декодер AV1 в сборке, соревноваться не с кем, но
// число сказано вслух: если однажды рядом окажется второй, выбор будет наш, а
// не случайный. У штатных плагинов libheif: aom 100, dav1d 150.
constexpr int kPriority = 150;

// Имя — НЕИЗМЕНЯЕМАЯ строка, и это не педантизм. У штатных плагинов libheif
// (decoder_aom.cc, decoder_dav1d.cc) get_plugin_name() пишет в общий
// `static char plugin_name[80]`, а libheif декодирует плитки в НЕСКОЛЬКИХ
// ПОТОКАХ сразу (image-items/grid.cc заводит до четырёх декодеров). Общий
// буфер там — гонка; повторять её незачем.
constexpr char kPluginName[] = "libgav1";

struct Decoder {
    Libgav1Decoder* ctx = nullptr;

    // ВХОД КОПИРУЕМ ОБЯЗАТЕЛЬНО. Libgav1DecoderEnqueueFrame НЕ копирует
    // данные и требует держать буфер живым до конца соответствующего
    // DequeueFrame, а буфер, который libheif подаёт в push_data, умирает
    // сразу после возврата. Кто здесь сэкономит, получит чтение
    // освобождённой памяти — в Release оно почти всегда «работает».
    std::vector<std::vector<uint8_t>> queued;
    size_t nextToEnqueue = 0;

    bool strict = false;
    std::string message;                       // текст ошибки переживает возврат
    const heif_security_limits* limits = nullptr;
};

heif_error makeError(heif_error_code code, heif_suberror_code sub, Decoder* d,
                     const char* text) {
    if (d) {
        d->message = text ? text : "";
        return heif_error{code, sub, d->message.c_str()};
    }
    return heif_error{code, sub, ""};
}

// Перевод состояний libgav1 в ошибки libheif. Отдельно стоят два состояния,
// которые ошибками НЕ являются: kStatusNothingToDequeue («кадра ещё нет,
// зови снова») и kStatusTryAgain («очередь полна»).
heif_error fromStatus(Decoder* d, Libgav1StatusCode rc) {
    heif_error_code code;
    heif_suberror_code sub = heif_suberror_Unspecified;
    switch (rc) {
        case kLibgav1StatusOk:
            return heif_error_ok;
        case kLibgav1StatusOutOfMemory:
        case kLibgav1StatusResourceExhausted:
            code = heif_error_Memory_allocation_error;
            break;
        case kLibgav1StatusBitstreamError:
            code = heif_error_Invalid_input;
            break;
        case kLibgav1StatusInvalidArgument:
            code = heif_error_Invalid_input;
            sub = heif_suberror_Invalid_parameter_value;
            break;
        case kLibgav1StatusUnimplemented:
            code = heif_error_Unsupported_feature;
            sub = heif_suberror_Unsupported_codec;
            break;
        default:
            code = heif_error_Decoder_plugin_error;
            break;
    }
    return makeError(code, sub, d, Libgav1GetErrorString(rc));
}

// --- обязанности плагина ---------------------------------------------------

const char* pluginName() { return kPluginName; }

int supportsFormat(heif_compression_format format) {
    return format == heif_compression_AV1 ? kPriority : 0;
}

int supportsFormat2(const heif_decoder_plugin_compressed_format_description* d) {
    return d ? supportsFormat(d->format) : 0;
}

heif_error newDecoder2(void** out, const heif_decoder_plugin_options* options) {
    if (!out) return heif_error{heif_error_Usage_error,
                                heif_suberror_Null_pointer_argument, ""};

    Decoder* d = new Decoder();

    Libgav1DecoderSettings settings;
    Libgav1DecoderSettingsInitDefault(&settings);

    // Один кадр на картинку — параллелить нечего. frame_parallel=1 потянул бы
    // за собой обязательный release_input_buffer и целый класс состояний
    // ради нулевой выгоды.
    settings.frame_parallel = 0;

    // Число потоков libheif доводит до нас сама (heif_decoding_options::
    // num_codec_threads → plugin_options::num_threads), заводить свой счётчик
    // не нужно. Ноль означает «решай сам».
    if (options && options->num_threads > 0) settings.threads = options->num_threads;

    if (options) {
        d->strict = options->strict_decoding != 0;
        d->limits = options->limits;
    }
    if (!d->limits) d->limits = heif_get_global_security_limits();

    const Libgav1StatusCode rc = Libgav1DecoderCreate(&settings, &d->ctx);
    if (rc != kLibgav1StatusOk) {
        const heif_error err = fromStatus(d, rc);
        // Сообщение живёт в d, а d сейчас умрёт: копируем в статику вызова.
        heif_error out_err{err.code, err.subcode, "libgav1: не удалось создать декодер"};
        delete d;
        return out_err;
    }

    *out = d;
    return heif_error_ok;
}

heif_error newDecoder(void** out) {
    heif_decoder_plugin_options options{};
    options.format = heif_compression_AV1;
    options.strict_decoding = 0;
    options.num_threads = 0;
    options.limits = heif_get_global_security_limits();
    return newDecoder2(out, &options);
}

void freeDecoder(void* raw) {
    Decoder* d = static_cast<Decoder*>(raw);
    if (!d) return;
    if (d->ctx) Libgav1DecoderDestroy(d->ctx);
    delete d;
}

void setStrictDecoding(void* raw, int flag) {
    if (Decoder* d = static_cast<Decoder*>(raw)) d->strict = flag != 0;
}

// Отдать декодеру всё, что он готов взять. Очередь НЕ опустошается: буферы
// должны пережить DequeueFrame, и живут они до freeDecoder.
heif_error feedPending(Decoder* d) {
    while (d->nextToEnqueue < d->queued.size()) {
        const std::vector<uint8_t>& buf = d->queued[d->nextToEnqueue];
        const Libgav1StatusCode rc = Libgav1DecoderEnqueueFrame(
            d->ctx, buf.data(), buf.size(), /*user_private_data=*/0,
            /*buffer_private_data=*/nullptr);
        if (rc == kLibgav1StatusTryAgain) break;   // сперва забрать готовое
        if (rc != kLibgav1StatusOk) return fromStatus(d, rc);
        ++d->nextToEnqueue;
    }
    return heif_error_ok;
}

heif_error pushData2(void* raw, const void* data, size_t size, uintptr_t) {
    Decoder* d = static_cast<Decoder*>(raw);
    if (!d) return heif_error{heif_error_Usage_error,
                              heif_suberror_Null_pointer_argument, ""};
    if (!data || size == 0) return heif_error_ok;

    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    d->queued.emplace_back(bytes, bytes + size);
    return feedPending(d);
}

heif_error pushData(void* raw, const void* data, size_t size) {
    return pushData2(raw, data, size, 0);
}

// SignalEOS здесь звать НЕЛЬЗЯ: в непараллельном режиме он освобождает все
// удерживаемые кадры, и указатель, полученный из DequeueFrame, протухает.
// Штатный плагин на aom тоже ничего здесь не делает.
heif_error flushData(void*) { return heif_error_ok; }

bool mapFormat(Libgav1ImageFormat format, heif_colorspace* cs, heif_chroma* chroma) {
    switch (format) {
        case kLibgav1ImageFormatYuv420:
            *cs = heif_colorspace_YCbCr; *chroma = heif_chroma_420; return true;
        case kLibgav1ImageFormatYuv422:
            *cs = heif_colorspace_YCbCr; *chroma = heif_chroma_422; return true;
        case kLibgav1ImageFormatYuv444:
            *cs = heif_colorspace_YCbCr; *chroma = heif_chroma_444; return true;
        case kLibgav1ImageFormatMonochrome400:
            *cs = heif_colorspace_monochrome; *chroma = heif_chroma_monochrome; return true;
    }
    return false;
}

heif_error decodeNextImage2(void* raw, heif_image** out_img, uintptr_t* out_user_data,
                            const heif_security_limits* limits) {
    Decoder* d = static_cast<Decoder*>(raw);
    if (!d || !out_img) return heif_error{heif_error_Usage_error,
                                          heif_suberror_Null_pointer_argument, ""};
    *out_img = nullptr;
    if (out_user_data) *out_user_data = 0;
    if (!limits) limits = d->limits;

    const Libgav1DecoderBuffer* frame = nullptr;
    for (;;) {
        const heif_error fed = feedPending(d);
        if (fed.code != heif_error_Ok) return fed;

        const Libgav1StatusCode rc = Libgav1DecoderDequeueFrame(d->ctx, &frame);

        // «Пока нечего» — не ошибка: libheif позовёт снова.
        if (rc == kLibgav1StatusNothingToDequeue) return heif_error_ok;
        if (rc == kLibgav1StatusTryAgain) {
            if (d->nextToEnqueue >= d->queued.size()) return heif_error_ok;
            continue;
        }
        if (rc != kLibgav1StatusOk) return fromStatus(d, rc);

        if (frame != nullptr) break;

        // Кадр разобран, но не показывается. Есть ещё неотданное — продолжаем,
        // иначе честно говорим «картинки нет» (без этой ветки был бы вечный цикл).
        if (d->nextToEnqueue >= d->queued.size()) return heif_error_ok;
    }

    // --- РАЗБОР ДАННЫХ, А ЗНАЧИТ ПРОВЕРКИ, А НЕ Q_ASSERT ------------------
    //
    // Битый AVIF — это обычный вход, а не дефект программы. Каждая проверка
    // ниже возвращает ошибку и НЕ роняет процесс.

    heif_colorspace colorspace;
    heif_chroma chroma;
    if (!mapFormat(frame->image_format, &colorspace, &chroma)) {
        return makeError(heif_error_Unsupported_feature, heif_suberror_Unsupported_data_version,
                         d, "libgav1: неизвестный формат кадра");
    }

    const int width = frame->displayed_width[0];
    const int height = frame->displayed_height[0];
    if (width <= 0 || height <= 0) {
        return makeError(heif_error_Invalid_input, heif_suberror_Invalid_image_size,
                         d, "libgav1: непригодный размер кадра");
    }

    // ПРЕДОХРАНИТЕЛЬ ДО ВЫДЕЛЕНИЯ. libheif сама сужает max_image_size_pixels
    // по объявленному в контейнере ispe перед тем, как позвать нас (см.
    // image-items/image_item.cc), — именно затем, чтобы плагин не выделил
    // больше обещанного. Проверяем ДО heif_image_create, а не после.
    //
    // ОСТАВШИЙСЯ ЗАЗОР НАЗЫВАЕТСЯ ВСЛУХ: у libgav1 нет настройки вроде
    // dav1d_settings::frame_size_limit, поэтому лживый поток успевает
    // заставить ЕЁ выделить кадр раньше, чем мы посмотрим. Закрывается только
    // своим распределителем кадров (Libgav1ComputeFrameBufferInfo +
    // Libgav1SetFrameBuffer) — это долг, а не сделанная работа.
    if (limits && limits->max_image_size_pixels) {
        const uint64_t pixels = static_cast<uint64_t>(width) * static_cast<uint64_t>(height);
        if (pixels > limits->max_image_size_pixels) {
            return makeError(heif_error_Memory_allocation_error, heif_suberror_Security_limit_exceeded,
                             d, "libgav1: кадр больше разрешённого предела");
        }
    }

    heif_image* img = nullptr;
    heif_error err = heif_image_create(width, height, colorspace, chroma, &img);
    if (err.code != heif_error_Ok) return err;

    // --- цвет -------------------------------------------------------------
    //
    // version=1 ставится ДО сеттеров. Штатный плагин на aom это делает, а на
    // dav1d — забыл; чужую забывчивость не повторяем.
    heif_color_profile_nclx nclx{};
    nclx.version = 1;
    HEIF_WARN_OR_FAIL(d->strict, img,
        heif_nclx_color_profile_set_color_primaries(
            &nclx, static_cast<uint16_t>(frame->color_primary)), {});
    HEIF_WARN_OR_FAIL(d->strict, img,
        heif_nclx_color_profile_set_transfer_characteristics(
            &nclx, static_cast<uint16_t>(frame->transfer_characteristics)), {});
    HEIF_WARN_OR_FAIL(d->strict, img,
        heif_nclx_color_profile_set_matrix_coefficients(
            &nclx, static_cast<uint16_t>(frame->matrix_coefficients)), {});
    nclx.full_range_flag = (frame->color_range == kLibgav1ColorRangeFull) ? 1 : 0;
    heif_image_set_nclx_color_profile(img, &nclx);

    // --- плоскости --------------------------------------------------------
    //
    // Размер каждой плоскости берём У libgav1 (displayed_width/height по
    // осям Y/U/V), а не пересчитываем (w+1)/2 руками, как это делают плагины
    // на aom и dav1d: на один класс ошибок меньше.
    static const heif_channel kChannels[3] = {
        heif_channel_Y, heif_channel_Cb, heif_channel_Cr};

    const int planes = (chroma == heif_chroma_monochrome) ? 1 : 3;
    const int bpp = frame->bitdepth;
    const int bytesPerPixel = (bpp + 7) / 8;

    for (int c = 0; c < planes; ++c) {
        const int pw = frame->displayed_width[c];
        const int ph = frame->displayed_height[c];
        if (pw <= 0 || ph <= 0 || frame->plane[c] == nullptr || frame->stride[c] <= 0) {
            heif_image_release(img);
            return makeError(heif_error_Invalid_input, heif_suberror_Invalid_image_size,
                             d, "libgav1: непригодная плоскость кадра");
        }

        err = heif_image_add_plane_safe(img, kChannels[c], pw, ph, bpp, limits);
        if (err.code != heif_error_Ok) {
            // heif_image сейчас умрёт, а вместе с ним и текст ошибки.
            d->message = err.message ? err.message : "";
            heif_error kept{err.code, err.subcode, d->message.c_str()};
            heif_image_release(img);
            return kept;
        }

        size_t dstStride = 0;
        uint8_t* dst = heif_image_get_plane2(img, kChannels[c], &dstStride);
        if (!dst) {
            heif_image_release(img);
            return makeError(heif_error_Memory_allocation_error, heif_suberror_Unspecified,
                             d, "libgav1: плоскость не выделилась");
        }

        const uint8_t* src = frame->plane[c];
        const size_t srcStride = static_cast<size_t>(frame->stride[c]);
        const size_t rowBytes = static_cast<size_t>(pw) * static_cast<size_t>(bytesPerPixel);
        for (int y = 0; y < ph; ++y) {
            std::memcpy(dst + static_cast<size_t>(y) * dstStride,
                        src + static_cast<size_t>(y) * srcStride, rowBytes);
        }
    }

    *out_img = img;
    return heif_error_ok;
}

heif_error decodeNextImage(void* raw, heif_image** out_img,
                           const heif_security_limits* limits) {
    return decodeNextImage2(raw, out_img, nullptr, limits);
}

heif_error decodeImage(void* raw, heif_image** out_img) {
    return decodeNextImage(raw, out_img, heif_get_global_security_limits());
}

const heif_decoder_plugin kPlugin{
    /* plugin_api_version */ 6,
    pluginName,
    /* init_plugin */ nullptr,
    /* deinit_plugin */ nullptr,
    supportsFormat,
    newDecoder,
    freeDecoder,
    pushData,
    decodeImage,
    setStrictDecoding,
    /* id_name */ "gav1",
    decodeNextImage,
    /* minimum_required_libheif_version */ LIBHEIF_MAKE_VERSION(1, 22, 0),
    supportsFormat2,
    newDecoder2,
    pushData2,
    flushData,
    decodeNextImage2,
};

}  // namespace

const heif_decoder_plugin* av1DecoderPlugin() { return &kPlugin; }

}  // namespace zametti
