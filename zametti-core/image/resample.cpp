#include "resample.h"

#include <QColorSpace>

#include <algorithm>
#include <cmath>
#include <vector>

namespace zametti {

namespace {

constexpr double kRadius = 3.0;   // Lanczos-3

double sinc(double x) {
    if (std::abs(x) < 1e-8) return 1.0;
    const double px = M_PI * x;
    return std::sin(px) / px;
}

double lanczos(double x) {
    const double a = std::abs(x);
    if (a >= kRadius) return 0.0;
    return sinc(x) * sinc(x / kRadius);
}

// Веса для одной оси. Для каждого выходного пикселя — список входных индексов
// и их долей. Считаем один раз на ось, а не на каждый пиксель: это разница
// между секундой и минутой на большой картинке.
struct Taps {
    int first = 0;              // индекс первого входного пикселя
    std::vector<float> weight;  // веса подряд, сумма равна единице
};

std::vector<Taps> buildTaps(int srcLen, int dstLen) {
    // Круглые скобки тут прочитались бы как объявление функции — самый
    // досадный разбор. Фигурные.
    std::vector<Taps> taps{static_cast<size_t>(dstLen)};
    const double scale = double(dstLen) / double(srcLen);
    // При УМЕНЬШЕНИИ окно фильтра растягивается: иначе часть входных пикселей
    // не попадёт ни в один выходной, и появится муар. При увеличении окно
    // остаётся своим.
    const double support = scale < 1.0 ? kRadius / scale : kRadius;
    const double step = scale < 1.0 ? scale : 1.0;

    for (int i = 0; i < dstLen; ++i) {
        // Центр выходного пикселя в координатах входа.
        const double center = (double(i) + 0.5) / scale - 0.5;
        int from = int(std::ceil(center - support));
        int to = int(std::floor(center + support));
        from = std::max(from, 0);
        to = std::min(to, srcLen - 1);
        if (to < from) {                    // вырожденный случай
            from = std::clamp(int(std::lround(center)), 0, srcLen - 1);
            to = from;
        }

        Taps t;
        t.first = from;
        t.weight.resize(size_t(to - from + 1));
        double sum = 0.0;
        for (int k = from; k <= to; ++k) {
            const double w = lanczos((double(k) - center) * step);
            t.weight[size_t(k - from)] = float(w);
            sum += w;
        }
        // Нормируем: без этого края картинки темнеют, потому что часть окна
        // выходит за границу и её веса теряются.
        if (sum != 0.0) {
            for (float& w : t.weight) w = float(double(w) / sum);
        } else {
            std::fill(t.weight.begin(), t.weight.end(), 0.0f);
            t.weight[0] = 1.0f;
        }
        taps[size_t(i)] = std::move(t);
    }
    return taps;
}

// Общий проход: два раздельных этапа по осям с готовыми весами.
QImage runTaps(const QImage& in, int width, int height, const std::vector<Taps>& tapsX,
               const std::vector<Taps>& tapsY) {
    const bool deep = in.depth() > 32;
    const double maxVal = deep ? 65535.0 : 255.0;
    const int sh = in.height();

    // Оба прохода раскладываются по строкам без единой связи между ними —
    // это и есть причина, по которой фильтр сделан разделимым. Параллелится
    // тривиально; на 60-мегапиксельной картинке разница в разы, и она нужна
    // и стенду подбора параметров, и живому импорту.
    //
    // OpenMP берётся, только если компилятор его даёт: на macOS с AppleClang
    // он требует отдельной libomp, а зависеть от неё мы не хотим. Без него всё
    // работает так же, только в один поток.
    std::vector<float> mid(size_t(width) * size_t(sh) * 4);
#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
    for (int y = 0; y < sh; ++y) {
        const uint8_t* row8 = in.constScanLine(y);
        const auto* row16 = reinterpret_cast<const uint16_t*>(row8);
        float* dst = mid.data() + size_t(y) * size_t(width) * 4;
        for (int x = 0; x < width; ++x) {
            const Taps& t = tapsX[size_t(x)];
            float acc[4] = {0, 0, 0, 0};
            for (size_t k = 0; k < t.weight.size(); ++k) {
                const size_t sx = size_t(t.first + int(k)) * 4;
                const float w = t.weight[k];
                if (deep) {
                    for (int c = 0; c < 4; ++c) acc[c] += w * float(row16[sx + size_t(c)]);
                } else {
                    for (int c = 0; c < 4; ++c) acc[c] += w * float(row8[sx + size_t(c)]);
                }
            }
            for (int c = 0; c < 4; ++c) dst[size_t(x) * 4 + size_t(c)] = acc[c];
        }
    }

    QImage out(width, height, in.format());
    if (out.isNull()) return {};
#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
    for (int y = 0; y < height; ++y) {
        const Taps& t = tapsY[size_t(y)];
        uint8_t* dst8 = out.scanLine(y);
        auto* dst16 = reinterpret_cast<uint16_t*>(dst8);
        for (int x = 0; x < width; ++x) {
            float acc[4] = {0, 0, 0, 0};
            for (size_t k = 0; k < t.weight.size(); ++k) {
                const float* s =
                    mid.data() + (size_t(t.first + int(k)) * size_t(width) + size_t(x)) * 4;
                const float w = t.weight[k];
                for (int c = 0; c < 4; ++c) acc[c] += w * s[size_t(c)];
            }
            // У Lanczos веса бывают отрицательными, и на резких границах
            // получается звон — значения выходят за диапазон. Обрезаем. У
            // усреднения по площади веса неотрицательны, и обрезать нечего.
            for (int c = 0; c < 4; ++c) {
                const double v = std::clamp(double(acc[c]), 0.0, maxVal);
                if (deep) dst16[size_t(x) * 4 + size_t(c)] = uint16_t(std::lround(v));
                else dst8[size_t(x) * 4 + size_t(c)] = uint8_t(std::lround(v));
            }
        }
    }
    out.setColorSpace(in.colorSpace());
    return out;
}

// Привести к формату, с которым умеют работать оба прохода.
QImage normalized(const QImage& src) {
    const bool deep = src.depth() > 32;
    const bool alpha = src.hasAlphaChannel();
    const QImage::Format fmt = deep ? (alpha ? QImage::Format_RGBA64 : QImage::Format_RGBX64)
                                    : (alpha ? QImage::Format_RGBA8888
                                             : QImage::Format_RGBX8888);
    return src.format() == fmt ? src : src.convertToFormat(fmt);
}

}  // namespace

QImage resampleLanczos(const QImage& src, int width, int height) {
    if (src.isNull() || width <= 0 || height <= 0) return {};
    if (src.width() == width && src.height() == height) return src;

    const QImage in = normalized(src);
    if (in.isNull()) return {};
    return runTaps(in, width, height, buildTaps(in.width(), width),
                   buildTaps(in.height(), height));
}

}  // namespace zametti
