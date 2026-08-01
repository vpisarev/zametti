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

QImage decodeJxl(const QByteArray& jxl) {
    QBuffer buf;
    buf.setData(jxl);
    buf.open(QIODevice::ReadOnly);
    QImageReader reader(&buf, "jxl");
    return reader.read();
}

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

LadderResult runLadder(const QImage& image, const EncodeMeta& meta, const ImportLimits& limits) {
    LadderResult out;
    if (image.isNull()) {
        out.error = QStringLiteral("нечего сжимать");
        return out;
    }

    const qint64 budget = limits.fileBudgetBytes();
    const qint64 slack = qint64(double(budget) * kBudgetSlack);

    EncodeOptions opt;
    opt.quality = limits.quality;
    opt.maxBitsPerChannel = limits.maxBitsPerChannel;

    QImage current = image;
    QString err;

    // --- ступень 1: как есть, обычным качеством ---------------------------
    QByteArray best = encodeJxl(current, opt, meta, &err);
    ++out.encodes;
    if (best.isEmpty()) {
        out.error = err;
        return out;
    }
    if (best.size() <= slack) {
        out.bytes = best;
        out.width = current.width();
        out.height = current.height();
        out.quality = opt.quality;
        return out;
    }

    // Дальше идёт отступление, и на каждой ступени мы помним ПРЕДЫДУЩИЙ
    // результат: арбитру может понадобиться вернуться.
    QByteArray previous = best;
    QImage previousImage = current;
    int previousQuality = opt.quality;

    // --- ступень 2: шаг качества вниз -------------------------------------
    opt.quality = std::max(kQualityFloor, opt.quality - kQualityStep);
    ++out.steps;
    best = encodeJxl(current, opt, meta, &err);
    ++out.encodes;
    if (best.isEmpty()) {
        out.error = err;
        return out;
    }

    // --- ступень 3: шаг разрешения ----------------------------------------
    if (best.size() > slack) {
        // Во сколько раз перелёт — во столько же уменьшаем ПЛОЩАДЬ, то есть
        // сторону в корень из этого. Так одним шагом попадаем примерно в
        // бюджет, а не спускаемся по чуть-чуть.
        const double over = double(best.size()) / double(budget);
        const double k = 1.0 / std::sqrt(over);
        const int w = std::max(1, int(std::lround(current.width() * k)));
        const int h = std::max(1, int(std::lround(current.height() * k)));
        previous = best;
        previousImage = current;
        previousQuality = opt.quality;

        current = resampleArea(current, w, h);
        ++out.steps;
        best = encodeJxl(current, opt, meta, &err);
        ++out.encodes;
        if (best.isEmpty()) {
            out.error = err;
            return out;
        }
    }

    // --- ступень 4: пол качества ------------------------------------------
    if (best.size() > slack && opt.quality > kQualityFloor) {
        previous = best;
        previousImage = current;
        previousQuality = opt.quality;
        opt.quality = kQualityFloor;
        ++out.steps;
        out.hitQualityFloor = true;
        best = encodeJxl(current, opt, meta, &err);
        ++out.encodes;
        if (best.isEmpty()) {
            out.error = err;
            return out;
        }
    }

    out.bytes = best;
    out.width = current.width();
    out.height = current.height();
    out.quality = opt.quality;
    out.overBudget = best.size() > budget;

    // --- ступень 5: арбитр -------------------------------------------------
    //
    // Зовём, только если отступали: если картинка влезла сразу, оценивать
    // нечего, а стоит арбитр дорого — на 2.56 Мп около полусекунды, то есть
    // как четыре энкода.
    if (out.steps == 0) return out;

    const QImage decoded = decodeJxl(best);
    if (decoded.isNull()) return out;
    // Сравнивать надо с картинкой ТОГО ЖЕ размера: если лестница уменьшала,
    // берём уменьшенный вход, а не исходный.
    out.ssimulacra2 = compareSsimulacra2(current, decoded);

    if (out.ssimulacra2 >= kArbiterAccept) return out;
    if (out.ssimulacra2 > kArbiterReject) return out;   // между порогами — терпим

    // Хуже нижнего порога: возвращаемся на ступень назад и принимаем перелёт
    // бюджета. Качество дороже мегабайта.
    out.bytes = previous;
    out.width = previousImage.width();
    out.height = previousImage.height();
    out.quality = previousQuality;
    out.overBudget = previous.size() > budget;
    out.arbiterRolledBack = true;
    return out;
}

}  // namespace zametti
