#include "webp_read.h"

#include <webp/decode.h>
#include <webp/demux.h>

#include <cstring>

namespace zametti {

bool looksLikeWebp(const QByteArray& head) {
    // Двенадцать байт: "RIFF", четыре байта длины, "WEBP". Длину не проверяем —
    // у оборванного файла она врёт, а опознание должно работать и по огрызку.
    if (head.size() < 12) return false;
    return std::memcmp(head.constData(), "RIFF", 4) == 0 &&
           std::memcmp(head.constData() + 8, "WEBP", 4) == 0;
}

QSize readWebpSize(const QByteArray& bytes) {
    if (!looksLikeWebp(bytes)) return {};
    int width = 0;
    int height = 0;
    if (!WebPGetInfo(reinterpret_cast<const uint8_t*>(bytes.constData()), size_t(bytes.size()),
                     &width, &height))
        return {};
    return QSize(width, height);
}

namespace {

// Первый кадр анимации. Простой WebPDecode* у анимированного файла не читает
// НИЧЕГО — не отдаёт даже первый кадр, — поэтому для него отдельная дверь.
// Заметке нужен кадр, а не кино: берём первый и на этом заканчиваем.
QImage firstFrameOfAnimation(const QByteArray& bytes) {
    WebPData data{reinterpret_cast<const uint8_t*>(bytes.constData()), size_t(bytes.size())};
    WebPDemuxer* demux = WebPDemux(&data);
    if (demux == nullptr) return {};

    QImage out;
    WebPIterator iter{};
    if (WebPDemuxGetFrame(demux, 1, &iter)) {
        int width = 0;
        int height = 0;
        if (WebPGetInfo(iter.fragment.bytes, iter.fragment.size, &width, &height)) {
            // У кадра анимации альфа есть почти всегда: ею склеивают кадры.
            QImage frame(width, height, QImage::Format_RGBA8888);
            if (!frame.isNull() &&
                WebPDecodeRGBAInto(iter.fragment.bytes, iter.fragment.size, frame.bits(),
                                   size_t(frame.sizeInBytes()),
                                   int(frame.bytesPerLine())) != nullptr) {
                out = frame;
            }
        }
        WebPDemuxReleaseIterator(&iter);
    }
    WebPDemuxDelete(demux);
    return out;
}

}  // namespace

QImage decodeWebp(const QByteArray& bytes) {
    const QSize size = readWebpSize(bytes);
    if (size.isEmpty()) return {};

    // Альфу спрашиваем у самого файла: у webp без альфы четвёртый канал всё
    // равно придёт единицами, но формат картинки лучше назвать честно — иначе
    // Qt будет таскать за ней смешивание, которого не требуется.
    WebPBitstreamFeatures features{};
    const bool haveFeatures =
        WebPGetFeatures(reinterpret_cast<const uint8_t*>(bytes.constData()), size_t(bytes.size()),
                        &features) == VP8_STATUS_OK;
    const bool alpha = haveFeatures && features.has_alpha != 0;

    QImage out(size, alpha ? QImage::Format_RGBA8888 : QImage::Format_RGBX8888);
    if (out.isNull()) return {};   // потолок памяти сказал «нет»

    // Пишем прямо в строки QImage: шаг у неё свой, и он честно передаётся
    // декодеру. Промежуточного буфера не заводим вовсе.
    const uint8_t* ok = WebPDecodeRGBAInto(
        reinterpret_cast<const uint8_t*>(bytes.constData()), size_t(bytes.size()), out.bits(),
        size_t(out.sizeInBytes()), int(out.bytesPerLine()));
    if (ok != nullptr) return out;

    // Не вышло — почти наверняка анимация: у неё кадры лежат в контейнере, и
    // простой декодер до них не добирается.
    return firstFrameOfAnimation(bytes);
}

}  // namespace zametti
