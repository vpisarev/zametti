#include "jpeg_read.h"

#include <QtGlobal>

#include <csetjmp>
#include <cstdio>
#include <cstring>

// Порядок важен: jpegli реализует API libjpeg, и его собственные заголовки
// ждут, что типы libjpeg уже объявлены.
#include <jpeglib.h>

#include "lib/jpegli/decode.h"
#include "lib/jpegli/types.h"

namespace zametti {

namespace {

// libjpeg сообщает о беде длинным прыжком: его обработчик ошибки не имеет
// права вернуть управление. Свой обработчик прыгает сюда, и разжатие
// заканчивается не падением, а пустой картинкой.
struct JumpingError {
    jpeg_error_mgr base;
    std::jmp_buf landing;
};

[[noreturn]] void onFatal(j_common_ptr info) {
    auto* err = reinterpret_cast<JumpingError*>(info->err);
    std::longjmp(err->landing, 1);
}

// Жалобы («лишние байты перед маркером», «повреждённые данные») печатать
// незачем: они сыплются на каждый второй снимок из интернета и человеку
// ничего не говорят. Картинка при этом читается.
void onWhine(j_common_ptr, int) {}

// Во сколько восьмых просить декодер уменьшить. libjpeg умеет N/8 при N от 1
// до 16; берём наибольшее уменьшение, при котором обе стороны ещё не меньше
// запрошенного предела. Точную подгонку оставляем вызывающему: у него свой
// сглаживатель, и он лучше нашего.
int eighthsFor(QSize full, QSize hint) {
    if (hint.isEmpty() || full.isEmpty()) return 8;
    for (int n = 1; n <= 8; ++n) {
        const int w = (full.width() * n + 7) / 8;
        const int h = (full.height() * n + 7) / 8;
        if (w >= hint.width() && h >= hint.height()) return n;
    }
    return 8;
}

}  // namespace

bool looksLikeJpeg(const QByteArray& head) {
    if (head.size() < 3) return false;
    const auto* p = reinterpret_cast<const unsigned char*>(head.constData());
    return p[0] == 0xFF && p[1] == 0xD8 && p[2] == 0xFF;
}

JpegHeader readJpegHeader(const QByteArray& bytes) {
    JpegHeader out;
    if (!looksLikeJpeg(bytes)) return out;

    jpeg_decompress_struct cinfo{};
    JumpingError err{};
    cinfo.err = jpegli_std_error(&err.base);
    err.base.error_exit = onFatal;
    err.base.emit_message = onWhine;

    if (setjmp(err.landing)) {
        jpegli_destroy_decompress(&cinfo);
        return JpegHeader{};
    }

    jpegli_create_decompress(&cinfo);
    jpegli_mem_src(&cinfo, reinterpret_cast<const unsigned char*>(bytes.constData()),
                   static_cast<unsigned long>(bytes.size()));
    if (jpegli_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
        jpegli_destroy_decompress(&cinfo);
        return JpegHeader{};
    }
    out.size = QSize(int(cinfo.image_width), int(cinfo.image_height));
    out.components = cinfo.num_components;
    out.progressive = cinfo.progressive_mode != 0;
    jpegli_destroy_decompress(&cinfo);
    return out;
}

QImage decodeJpeg(const QByteArray& bytes, QSize hint, JpegDepth depth) {
    if (!looksLikeJpeg(bytes)) return {};
    const bool deep = depth == JpegDepth::Sixteen;

    jpeg_decompress_struct cinfo{};
    JumpingError err{};
    cinfo.err = jpegli_std_error(&err.base);
    err.base.error_exit = onFatal;
    err.base.emit_message = onWhine;

    QImage out;
    if (setjmp(err.landing)) {
        jpegli_destroy_decompress(&cinfo);
        return {};   // недочитанное не отдаём: половина картинки хуже рамки
    }

    jpegli_create_decompress(&cinfo);
    jpegli_mem_src(&cinfo, reinterpret_cast<const unsigned char*>(bytes.constData()),
                   static_cast<unsigned long>(bytes.size()));
    if (jpegli_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
        jpegli_destroy_decompress(&cinfo);
        return {};
    }

    // Уменьшение — у декодера, до всякого разжатия.
    cinfo.scale_num = unsigned(eighthsFor(QSize(int(cinfo.image_width), int(cinfo.image_height)),
                                          hint));
    cinfo.scale_denom = 8;

    cinfo.out_color_space = JCS_RGB;
    jpegli_set_output_format(&cinfo, deep ? JPEGLI_TYPE_UINT16 : JPEGLI_TYPE_UINT8,
                             JPEGLI_NATIVE_ENDIAN);

    if (!jpegli_start_decompress(&cinfo)) {
        jpegli_destroy_decompress(&cinfo);
        return {};
    }

    const int width = int(cinfo.output_width);
    const int height = int(cinfo.output_height);
    if (width <= 0 || height <= 0) {
        jpegli_destroy_decompress(&cinfo);
        return {};
    }

    // Восемь бит — прямо в RGB888: три канала у декодера, три у Qt, и строка
    // отдаётся ему как есть, без перекладки. Шестнадцать — только RGBX64:
    // трёхканального шестнадцатибитного формата у Qt нет вовсе, поэтому
    // четвёртый канал приходится досыпать самим.
    out = QImage(width, height, deep ? QImage::Format_RGBX64 : QImage::Format_RGB888);
    if (out.isNull()) {   // потолок памяти сказал «нет»
        jpegli_destroy_decompress(&cinfo);
        return {};
    }

    if (!deep) {
        while (cinfo.output_scanline < cinfo.output_height) {
            auto* line = reinterpret_cast<JSAMPROW>(out.scanLine(int(cinfo.output_scanline)));
            if (jpegli_read_scanlines(&cinfo, &line, 1) != 1) break;
        }
    } else {
        QByteArray row(qsizetype(size_t(width) * 3u * sizeof(quint16)), Qt::Uninitialized);
        while (cinfo.output_scanline < cinfo.output_height) {
            auto* line = reinterpret_cast<JSAMPROW>(row.data());
            if (jpegli_read_scanlines(&cinfo, &line, 1) != 1) break;
            const auto* src = reinterpret_cast<const quint16*>(row.constData());
            auto* dst = reinterpret_cast<quint16*>(out.scanLine(int(cinfo.output_scanline) - 1));
            for (int x = 0; x < width; ++x) {
                dst[x * 4 + 0] = src[x * 3 + 0];
                dst[x * 4 + 1] = src[x * 3 + 1];
                dst[x * 4 + 2] = src[x * 3 + 2];
                dst[x * 4 + 3] = 0xFFFF;
            }
        }
    }
    const bool whole = cinfo.output_scanline >= cinfo.output_height;

    jpegli_finish_decompress(&cinfo);
    jpegli_destroy_decompress(&cinfo);
    return whole ? out : QImage();
}

}  // namespace zametti
