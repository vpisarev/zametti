#include "ladder.h"

#include "resample.h"

#include "tools/ssimulacra2.h"
#include "lib/jxl/color_encoding_internal.h"
#include "lib/jxl/image.h"
#include "lib/jxl/image_bundle.h"
#include "tools/no_memory_manager.h"

#include <jxl/decode.h>
#include <jxl/decode_cxx.h>

#include <QBuffer>
#include <QColorSpace>
#include <QImageReader>
#include <QPainter>

#include <algorithm>
#include <cmath>
#include <memory>

namespace zametti {

namespace {


// Наложить на серый фон, если есть прозрачность. Серый, а не чёрный или белый:
// крайние цвета сдвинули бы оценку в тех местах, где прозрачное граничит с
// непрозрачным.
QImage flatten(const QImage& src) {
    if (!src.hasAlphaChannel()) return src;
    QImage out(src.size(), QImage::Format_RGBX8888);
    out.fill(QColor(128, 128, 128));
    QPainter p(&out);
    p.drawImage(0, 0, src);
    p.end();
    out.setColorSpace(src.colorSpace());
    return out;
}

std::shared_ptr<jxl::ImageBundle> toBundle(const QImage& src) {
    const QImage img = flatten(src).convertToFormat(QImage::Format_RGBX64);
    JxlMemoryManager* mm = jpegxl::tools::NoMemoryManager();
    auto created = jxl::Image3F::Create(mm, size_t(img.width()), size_t(img.height()));
    if (!created.ok()) return nullptr;
    jxl::Image3F out = std::move(created).value_();
    for (int y = 0; y < img.height(); ++y) {
        const auto* s = reinterpret_cast<const uint16_t*>(img.constScanLine(y));
        float* r = out.PlaneRow(0, size_t(y));
        float* g = out.PlaneRow(1, size_t(y));
        float* b = out.PlaneRow(2, size_t(y));
        for (int x = 0; x < img.width(); ++x) {
            r[x] = s[x * 4 + 0] / 65535.0f;
            g[x] = s[x * 4 + 1] / 65535.0f;
            b[x] = s[x * 4 + 2] / 65535.0f;
        }
    }
    auto ib = std::make_shared<jxl::ImageBundle>(mm, new jxl::ImageMetadata());
    if (!ib->SetFromImage(std::move(out), jxl::ColorEncoding::SRGB(false))) return nullptr;
    return ib;
}

}  // namespace

double compareSsimulacra2(const QImage& a, const QImage& b) {
    if (a.isNull() || b.isNull() || a.size() != b.size()) return -1000.0;
    auto x = toBundle(a);
    auto y = toBundle(b);
    if (!x || !y) return -1000.0;
    auto res = ComputeSSIMULACRA2(*x, *y);
    if (!res.ok()) return -1000.0;
    return std::move(res).value_().Score();
}

}  // namespace zametti
