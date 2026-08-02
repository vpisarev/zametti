#include "import_limits.h"

#include <algorithm>
#include <cmath>

namespace zametti {

double decodedBytes(int width, int height, int bitsPerSample) {
    if (width <= 0 || height <= 0) return 0.0;
    const double perSample = bitsPerSample > 8 ? 2.0 : 1.0;
    return double(width) * double(height) * 4.0 * perSample;
}

Refusal checkSource(int width, int height, int bitsPerSample, const ImportLimits& limits) {
    // «Читатель не смог назвать размер» — отдельная ветка, а не площадь от
    // минус единицы. Наступал: у бомбы 10^9 x 1 libpng бракует заголовок сам,
    // и Qt возвращает -1 x -1. Считать площадь от этого бессмысленно.
    if (width < 0 || height < 0) return Refusal::NoSize;
    if (width == 0 || height == 0) return Refusal::Empty;

    // Стороны РАНЬШЕ площади: после этой проверки площадь заведомо не больше
    // 65535² = 4.3e9, и переполниться уже негде.
    if (width > kMaxSide || height > kMaxSide) return Refusal::SideTooBig;

    const double area = double(width) * double(height);
    if (area > kMaxArea) return Refusal::AreaTooBig;

    if (decodedBytes(width, height, bitsPerSample) > limits.decodeBudgetBytes())
        return Refusal::TooMuchMemory;

    return Refusal::None;
}

Size targetSize(Size src, const ImportLimits& limits) {
    if (src.width <= 0 || src.height <= 0) return src;
    const double s = double(limits.maxSize);
    if (s <= 0) return src;

    const double budget = s * s;      // бюджет площади
    const double cap = 3.0 * s;       // потолок стороны
    const double w0 = double(src.width);
    const double h0 = double(src.height);

    double k = std::sqrt(budget / (w0 * h0));
    k = std::min(k, cap / std::max(w0, h0));

    // Больше единицы — картинка мельче бюджета. Растягивать её нельзя: пикселей
    // от этого не прибавится, а весить она станет вчетверо.
    if (k >= 1.0) return src;

    Size out;
    out.width = std::max(1, int(std::lround(w0 * k)));
    out.height = std::max(1, int(std::lround(h0 * k)));
    return out;
}


}  // namespace zametti
