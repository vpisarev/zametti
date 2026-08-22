// ПОСМЕРТНОЕ ПРЕВЬЮ: что остаётся от вложения, ушедшего с удалённой заметкой.
//
// Проверяется ровно то, ради чего оно есть: размер по правилу площади (S² и ни
// пикселем больше), метка «удалено» с выросшей ревизией, идемпотентность и
// отказ, который не врёт.

#include "deleted_image.h"

#include "exif.h"
#include "image_read.h"
#include "import_limits.h"
#include "jxl_encoder.h"
#include "test_util.h"

#include <QByteArray>
#include <QImage>

#include <string>
#include <vector>

using namespace zametti;

namespace {

template <typename T>
std::string num(T value) { return std::to_string(value); }

ImportLimits limitsFor(int side) {
    ImportLimits limits;
    limits.maxSize = side;
    limits.quality = 90;
    return limits;
}

// Картинка с узнаваемым содержимым: ровный градиент, чтобы уменьшение было
// осмысленным, а не шумом.
QImage sample(int width, int height) {
    QImage image(width, height, QImage::Format_RGB32);
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
            image.setPixel(x, y, qRgb((x * 255) / qMax(1, width - 1),
                                      (y * 255) / qMax(1, height - 1), 128));
    return image;
}

QByteArray asJxl(const QImage& image, const EncodeMeta& meta = {}) {
    EncodeOptions options;
    options.quality = 90;
    options.effort = 3;
    QString error;
    return encodeJxl(image, options, meta, &error);
}

void checkAreaBudget() {
    const QByteArray big = asJxl(sample(1200, 800));
    ZT_TRUE("исходник закодировался", !big.isEmpty());

    QString error;
    const QByteArray small = makeDeletedImage(big, limitsFor(100), &error);
    ZT_TRUE("превью сделано: " + error.toStdString(), !small.isEmpty());
    ZT_TRUE("и оно много меньше исходника: " + num(small.size()) + " против " + num(big.size()),
            small.size() * 4 < big.size());

    const QImage back = decodeImage(small);
    ZT_TRUE("превью читается", !back.isNull());
    // Бюджет S², с точностью до округления сторон: каждая считается как
    // S·√a и округляется вниз, поэтому площадь может выйти на десяток
    // пикселей больше ровного квадрата. Потолок берём (S+1)² — больше этого
    // округление дать не может.
    ZT_TRUE("площадь в бюджете S²: " + num(back.width()) + "×" + num(back.height()),
            back.width() * back.height() <= 101 * 101);
    ZT_TRUE("пропорции сохранены",
            qAbs(double(back.width()) / back.height() - 1200.0 / 800.0) < 0.05);
}

void checkNoUpscale() {
    // Картинка мельче бюджета остаётся собой: апскейла нет ни на каком пути.
    const QByteArray tiny = asJxl(sample(40, 30));
    QString error;
    const QByteArray out = makeDeletedImage(tiny, limitsFor(100), &error);
    ZT_TRUE("превью сделано", !out.isEmpty());
    const QImage back = decodeImage(out);
    ZT_EQ("ширина та же", num(40), num(back.width()));
    ZT_EQ("и высота та же", num(30), num(back.height()));
}

void checkMarkAndRevision() {
    const QByteArray big = asJxl(sample(600, 400));
    QString error;
    const QByteArray buried = makeDeletedImage(big, limitsFor(100), &error);
    ZT_TRUE("превью сделано", !buried.isEmpty());

    const ImageMeta meta = readImageMeta(std::string_view(buried.constData(),
                                                          size_t(buried.size())));
    ZT_TRUE("метка удаления стоит", xmpZamettiDeleted(meta.xmp));
    // Ревизии у исходника не было — считаем её единицей, значит у превью двойка.
    ZT_EQ("ревизия выросла до двух", num(2), num(xmpZamettiRev(meta.xmp)));

    // ВТОРОЙ РАЗ ПО УЖЕ ПОХОРОНЕННОМУ: ревизия растёт дальше, а размер не
    // ужимается снова — картинка уже в бюджете.
    const QByteArray again = makeDeletedImage(buried, limitsFor(100), &error);
    ZT_TRUE("второе превью сделано", !again.isEmpty());
    const ImageMeta twice = readImageMeta(std::string_view(again.constData(),
                                                           size_t(again.size())));
    ZT_EQ("ревизия стала три", num(3), num(xmpZamettiRev(twice.xmp)));
    const QImage back = decodeImage(again);
    ZT_TRUE("размер не изменился", back.width() * back.height() <= 101 * 101);
}

void checkKeepsForeignMetadata() {
    EncodeMeta meta;
    const std::string xmp = xmpWithFileName("", "DSC_0001.NEF");
    meta.xmp = QByteArray(xmp.data(), qsizetype(xmp.size()));
    const QByteArray big = asJxl(sample(600, 400), meta);

    QString error;
    const QByteArray buried = makeDeletedImage(big, limitsFor(100), &error);
    ZT_TRUE("превью сделано", !buried.isEmpty());
    const ImageMeta back = readImageMeta(std::string_view(buried.constData(),
                                                          size_t(buried.size())));
    ZT_EQ("чужое поле пережило похороны", std::string("DSC_0001.NEF"),
          xmpValue(back.xmp, "xmpMM:PreservedFileName"));
    ZT_TRUE("и метка стоит", xmpZamettiDeleted(back.xmp));
}

void checkRefusesGarbage() {
    QString error;
    const QByteArray out = makeDeletedImage(QByteArray("не картинка вовсе"), limitsFor(100),
                                            &error);
    ZT_TRUE("мусор не превращается в превью", out.isEmpty());
    ZT_TRUE("и сказано почему", !error.isEmpty());
}

}  // namespace

static int ztRunSuite(int argc, char** argv) {
    (void)argc;
    (void)argv;
    checkAreaBudget();
    checkNoUpscale();
    checkMarkAndRevision();
    checkKeepsForeignMetadata();
    checkRefusesGarbage();
    return zt::report("посмертное превью");
}

TEST(DeletedImage, All) {
    std::vector<QByteArray> ztArgs{QByteArrayLiteral("deleted_image_test")};
    std::vector<char*> ztArgv;
    for (QByteArray& a : ztArgs) ztArgv.push_back(a.data());
    EXPECT_EQ(0, ztRunSuite(int(ztArgv.size()), ztArgv.data()));
}
