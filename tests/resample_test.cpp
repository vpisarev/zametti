// Ресемплер Lanczos-3: проверяется свойствами, а не картинками.
//
// «Похоже на правду» тут не годится: ошибка в весах даёт картинку, которая
// выглядит нормально и при этом темнее оригинала или сдвинута на полпикселя.
// Поэтому проверяются вещи, которые обязаны выполняться точно.

#include "resample.h"

#include "test_util.h"

#include <vector>
#include "testdata.h"

#include <QColorSpace>
#include <QGuiApplication>
#include <QImage>

#include <cmath>
#include <string>

using namespace zametti;

namespace {

std::string num(long long v) { return std::to_string(v); }

QImage solid(int w, int h, int r, int g, int b, bool deep = false) {
    QImage img(w, h, deep ? QImage::Format_RGBX64 : QImage::Format_RGBX8888);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            if (deep) {
                auto* p = reinterpret_cast<uint16_t*>(img.scanLine(y)) + size_t(x) * 4;
                p[0] = uint16_t(r * 257); p[1] = uint16_t(g * 257);
                p[2] = uint16_t(b * 257); p[3] = 65535;
            } else {
                auto* p = img.scanLine(y) + size_t(x) * 4;
                p[0] = uint8_t(r); p[1] = uint8_t(g); p[2] = uint8_t(b); p[3] = 255;
            }
        }
    }
    return img;
}

double meanChannel(const QImage& img, int channel) {
    // ИМЕННО RGBA, а не RGBX: приведение к формату без альфы выставляет её в
    // 255, и проверка альфы читала бы не то (наступал).
    const QImage in = img.convertToFormat(QImage::Format_RGBA8888);
    double sum = 0;
    for (int y = 0; y < in.height(); ++y)
        for (int x = 0; x < in.width(); ++x)
            sum += in.constScanLine(y)[size_t(x) * 4 + size_t(channel)];
    return sum / (double(in.width()) * in.height());
}

void checkIdentity() {
    const QImage src = solid(37, 23, 10, 200, 90);
    const QImage same = resampleLanczos(src, 37, 23);
    ZT_EQ("тот же размер — та же картинка", num(0),
          num(std::memcmp(src.constBits(), same.constBits(), size_t(src.sizeInBytes()))));
}

void checkFlatStaysFlat() {
    // САМАЯ ВАЖНАЯ ПРОВЕРКА. Одноцветная картинка обязана остаться той же
    // яркости при любом изменении размера. Если веса не нормированы, края
    // темнеют; если центр смещён — появляется градиент. И то и другое на
    // фотографии заметишь не сразу, а здесь видно сразу.
    for (auto size : {std::pair{100, 60}, std::pair{800, 480}, std::pair{7, 5}}) {
        const QImage src = solid(200, 120, 40, 130, 220);
        const QImage out = resampleLanczos(src, size.first, size.second);
        const std::string what = " (" + num(size.first) + "x" +
                                 num(size.second) + ")";
        ZT_TRUE("размер тот, что просили" + what,
                out.width() == size.first && out.height() == size.second);
        for (int c = 0; c < 3; ++c) {
            const double want = c == 0 ? 40 : (c == 1 ? 130 : 220);
            const double got = meanChannel(out, c);
            ZT_TRUE("канал " + num(c) + " не поплыл" + what + ": " +
                        std::to_string(int(got)) + " против " + std::to_string(int(want)),
                    std::abs(got - want) < 1.0);
        }
        // И ни один пиксель не должен отличаться от заливки: у ровного поля
        // Lanczos обязан давать ровное поле, без звона.
        const QImage flat = out.convertToFormat(QImage::Format_RGBX8888);
        int worst = 0;
        for (int y = 0; y < flat.height(); ++y)
            for (int x = 0; x < flat.width(); ++x)
                worst = std::max(worst, std::abs(int(flat.constScanLine(y)[size_t(x) * 4]) - 40));
        ZT_TRUE("на заливке нет звона" + what + " (худший пиксель " + num(worst) + ")",
                worst <= 1);
    }
}

void checkEdgesNotDark() {
    // Край — то место, где окно фильтра выходит за картинку. Без нормировки
    // весов он темнеет; проверяем углы отдельно от середины.
    const QImage src = solid(300, 300, 200, 200, 200);
    const QImage out = resampleLanczos(src, 90, 90).convertToFormat(QImage::Format_RGBX8888);
    const int corner = out.constScanLine(0)[0];
    const int middle = out.constScanLine(45)[45 * 4];
    ZT_TRUE("угол не темнее середины (" + num(corner) + " против " + num(middle) + ")",
            std::abs(corner - middle) <= 1);
}

void checkDeepAndAlpha() {
    const QImage deep = solid(120, 80, 33, 160, 250, true);
    const QImage out = resampleLanczos(deep, 60, 40);
    ZT_TRUE("глубина сохранена", out.depth() == 64);
    ZT_TRUE("глубокая заливка не поплыла", std::abs(meanChannel(out, 1) - 160) < 1.5);

    QImage withAlpha(64, 64, QImage::Format_RGBA8888);
    withAlpha.fill(QColor(10, 20, 30, 128));
    const QImage scaled = resampleLanczos(withAlpha, 32, 32);
    ZT_TRUE("альфа осталась", scaled.hasAlphaChannel());
    ZT_TRUE("и её значение сохранилось",
            std::abs(meanChannel(scaled, 3) - 128) < 1.5);
}

void checkColorSpaceKept() {
    QImage src = solid(80, 80, 100, 100, 100);
    src.setColorSpace(QColorSpace(QColorSpace::DisplayP3));
    const QImage out = resampleLanczos(src, 40, 40);
    ZT_TRUE("пространство не потерялось", out.colorSpace() == QColorSpace(QColorSpace::DisplayP3));
}

void checkDegenerate() {
    const QImage src = solid(10, 10, 1, 2, 3);
    ZT_TRUE("нулевая ширина — пусто", resampleLanczos(src, 0, 5).isNull());
    ZT_TRUE("пустой вход — пусто", resampleLanczos(QImage(), 5, 5).isNull());
    // Уменьшение до одного пикселя: след покрывает всю картинку.
    const QImage one = resampleLanczos(src, 1, 1);
    ZT_TRUE("до одного пикселя ужимается", one.width() == 1 && one.height() == 1);
    // Сильное увеличение: тоже не должно рушиться.
    const QImage big = resampleLanczos(src, 200, 200);
    ZT_TRUE("сильное увеличение проходит", big.width() == 200);
    ZT_TRUE("и цвет держится", std::abs(meanChannel(big, 1) - 2) < 1.5);
}

}  // namespace

static int ztRunSuite(int argc, char** argv) {
    (void)argc;
    (void)argv;
    checkIdentity();
    checkFlatStaysFlat();
    checkEdgesNotDark();
    checkDeepAndAlpha();
    checkColorSpaceKept();
    checkDegenerate();
    return zt::report("ресемплер Lanczos");
}

// Набор целиком одним TEST: тело не тронуто, argv ему собран здесь.
// Дробить на отдельные проверки — отдельная работа, по одному набору.
TEST(Resample, All) {
    std::vector<QByteArray> ztArgs{QByteArrayLiteral("resample_test")};
    std::vector<char*> ztArgv;
    for (QByteArray& a : ztArgs) ztArgv.push_back(a.data());
    EXPECT_EQ(0, ztRunSuite(int(ztArgv.size()), ztArgv.data()));
}
