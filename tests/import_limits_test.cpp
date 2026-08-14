// Границы входа и пиксельный бюджет.
//
// Здесь нет ни файлов, ни кодеков — одна арифметика, и потому проверить её
// можно насквозь, включая случаи, которых в природе не бывает. Ошибка тут
// стоит дороже прочих: пропущенная бомба съедает память до отказа, а неверный
// бюджет портит все картинки разом и необратимо.

#include "import_limits.h"

#include "test_util.h"

#include <cmath>
#include <string>

using namespace zametti;

namespace {

std::string num(long long v) { return std::to_string(v); }
std::string sz(Size s) { return num(s.width) + "x" + num(s.height); }

const char* refusalName(Refusal r) {
    switch (r) {
        case Refusal::None: return "принято";
        case Refusal::NoSize: return "размер не назван";
        case Refusal::Empty: return "нулевой размер";
        case Refusal::SideTooBig: return "сторона велика";
        case Refusal::AreaTooBig: return "площадь велика";
        case Refusal::TooMuchMemory: return "не влезает в память";
    }
    return "?";
}

void checkRefusal(const std::string& what, Refusal expected, int w, int h, int bits,
                  const ImportLimits& limits) {
    const Refusal got = checkSource(w, h, bits, limits);
    ZT_EQ(what, std::string(refusalName(expected)), std::string(refusalName(got)));
}

void checkBoundaries() {
    ImportLimits limits;   // умолчания; границы входа от S не зависят

    checkRefusal("обычное фото проходит", Refusal::None, 4032, 3024, 8, limits);
    checkRefusal("крошечная проходит", Refusal::None, 120, 120, 8, limits);

    // «Читатель не смог назвать размер» — отдельная ветка. Наступал на этом:
    // у бомбы 10^9 x 1 libpng бракует заголовок сам, и Qt отдаёт -1 x -1;
    // считать площадь от минус единицы бессмысленно.
    checkRefusal("минус единица — не размер", Refusal::NoSize, -1, -1, 8, limits);
    checkRefusal("ноль по стороне", Refusal::Empty, 0, 100, 8, limits);
    checkRefusal("ноль по высоте", Refusal::Empty, 100, 0, 8, limits);

    checkRefusal("сторона ровно на пределе проходит по стороне", Refusal::AreaTooBig, 65535,
                 65535, 8, limits);
    checkRefusal("сторона за пределом", Refusal::SideTooBig, 65536, 10, 8, limits);
    checkRefusal("сторона 70000", Refusal::SideTooBig, 70000, 100, 8, limits);

    // 600 Мп при законных сторонах — бьёт ровно в площадь.
    checkRefusal("площадь за пределом", Refusal::AreaTooBig, 30000, 20000, 8, limits);
    // 512 Мп ровно — на пределе, но не за ним.
    checkRefusal("площадь ровно на пределе проходит по площади", Refusal::TooMuchMemory, 32000,
                 16000, 8, limits);

    // САМОЕ ВАЖНОЕ ЧИСЛО НАБОРА. 65535² = 4 294 836 225, что переполняет int32
    // (предел 2 147 483 647): в тридцати двух битах площадь стала бы
    // отрицательной и прошла бы насквозь. А (2^32-1)² = 1.84e19 переполняет уже
    // int64 — оттого и считаем в double.
    const double huge = double(65535) * double(65535);
    ZT_TRUE("65535² переполняет int32", huge > 2147483647.0);
    ZT_TRUE("площадь считается без заворота", huge > kMaxArea);
}

void checkMemoryCeiling() {
    ImportLimits limits;
    limits.maxDecodeMemoryMb = 128;   // как было бы по первой редакции брифа

    // Собственный 16-битный TIFF владельца: 6000x4000 просит 183 МБ.
    checkRefusal("при потолке 128 МБ глубокий TIFF владельца отвергается",
                 Refusal::TooMuchMemory, 6000, 4000, 16, limits);
    // Он же восьмибитным просит 92 МБ и проходит — вот и вся разница.
    checkRefusal("он же восьмибитным проходит", Refusal::None, 6000, 4000, 8, limits);

    limits.maxDecodeMemoryMb = 1024;  // умолчание после правки
    checkRefusal("при потолке 1 ГБ проходит", Refusal::None, 6000, 4000, 16, limits);
    // 12-битный AVIF на 102 Мп просит 776 МБ — тоже влезает.
    checkRefusal("102 Мп глубокая влезает в гигабайт", Refusal::None, 8736, 11648, 12, limits);

    ZT_EQ("глубокая стоит вдвое дороже",
          num(static_cast<long long>(decodedBytes(1000, 1000, 8) * 2)),
          num(static_cast<long long>(decodedBytes(1000, 1000, 16))));
}

// Умолчания отдельной проверкой: они выведены замером (таблицы в
// app/settings.h), и молчаливая их смена — это молчаливая смена всего, что
// ляжет в хранилище. Пусть такая правка сначала покраснеет здесь.
void checkDefaults() {
    const ImportLimits d;
    ZT_EQ("S по умолчанию", num(2880), num(d.maxSize));
    ZT_EQ("качество по умолчанию", num(90), num(d.quality));
    ZT_EQ("потолок глубины по умолчанию", num(12), num(d.maxBitsPerChannel));
}

void checkTargetSize() {
    // S задаётся ЯВНО: здесь проверяется арифметика бюджета, а не умолчание.
    // За умолчание отвечает checkDefaults, и это разные вопросы — иначе смена
    // умолчания красила бы весь набор и прятала настоящую поломку.
    ImportLimits limits;
    limits.maxSize = 1600;   // бюджет 2.56 Мп, потолок стороны 4800

    // Квадрат ровно в бюджет.
    ZT_EQ("квадрат ужимается до стороны S", sz({1600, 1600}),
          sz(targetSize({4000, 4000}, limits)));

    // АПСКЕЙЛА НЕТ. Это правило, а не следствие: картинка мельче бюджета
    // остаётся собой.
    ZT_EQ("мелкая идёт как есть", sz({120, 120}), sz(targetSize({120, 120}, limits)));
    ZT_EQ("средняя идёт как есть", sz({1535, 1024}), sz(targetSize({1535, 1024}, limits)));

    // Панорама 3:1 живёт НА БЮДЖЕТЕ: ширина S·√3 = 2771.
    const Size pano = targetSize({8000, 2667}, limits);
    ZT_TRUE("панорама 3:1 около 2771 по ширине (" + num(pano.width) + ")",
            std::abs(pano.width - 2771) <= 3);
    ZT_TRUE("и площадь у неё около бюджета",
            std::abs(double(pano.width) * pano.height - 2.56e6) < 3.0e4);
    ZT_TRUE("потолок стороны не задет", pano.width <= 4800);

    // Соотношение 9:1 — ровно там, где бюджет упирается в потолок стороны.
    const Size nine = targetSize({9000, 1000}, limits);
    ZT_TRUE("при 9:1 ширина у потолка (" + num(nine.width) + ")",
            std::abs(nine.width - 4800) <= 3);

    // Лента 20:1 — режет уже потолок стороны, а не бюджет.
    const Size ribbon = targetSize({20000, 1000}, limits);
    ZT_EQ("лента 20:1 обрезана потолком стороны", num(4800), num(ribbon.width));
    ZT_TRUE("и площадь у неё МЕНЬШЕ бюджета",
            double(ribbon.width) * ribbon.height < 2.56e6);

    // Вертикальная панорама: потолок не привязан к ширине.
    const Size tall = targetSize({1856, 8192}, limits);
    ZT_TRUE("вертикальная обрезана по ВЫСОТЕ (" + num(tall.height) + ")", tall.height <= 4800);
    ZT_TRUE("и она выше, чем шире", tall.height > tall.width);

    // Пропорции сохраняются: это важнее круглых чисел.
    const Size photo = targetSize({4032, 3024}, limits);
    const double before = 4032.0 / 3024.0;
    const double after = double(photo.width) / photo.height;
    ZT_TRUE("пропорции сохранены", std::abs(before - after) < 0.01);

    // Вырожденное: нулевой S не должен ронять деление.
    ImportLimits zero;
    zero.maxSize = 0;
    ZT_EQ("нулевой бюджет ничего не меняет", sz({100, 50}), sz(targetSize({100, 50}, zero)));
}


}  // namespace

static int ztRunSuite() {
    checkBoundaries();
    checkMemoryCeiling();
    checkDefaults();
    checkTargetSize();
    return zt::report("границы входа и бюджет");
}

// Набор целиком одним TEST: тело не тронуто, argv ему собран здесь.
// Дробить на отдельные проверки — отдельная работа, по одному набору.
TEST(ImportLimits, All) {
    EXPECT_EQ(0, ztRunSuite());
}
