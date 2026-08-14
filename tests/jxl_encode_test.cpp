// Запись JXL: глубина, цвет, метаданные, транскод.
//
// Проверяется КРУГОМ: закодировали — прочитали своим же плагином — сверили с
// тем, что клали. Одного «не упало» мало: молча потерянная глубина или
// подменённый профиль выглядят точно так же, как успех, а в хранилище это
// осядет навсегда.

#include "image_read.h"
#include "jxl_encoder.h"
#include "exif.h"

#include "test_util.h"

#include <vector>
#include "testdata.h"

#include <QColorSpace>
#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QImageReader>
#include <QBuffer>

#include <string>

using namespace zametti;

namespace {

std::string num(long long v) { return std::to_string(v); }

// Картинка с плавным переходом: на ней видно и потерю разрядности (полосы), и
// подмену пространства.
QImage gradient(int w, int h, bool deep, bool alpha) {
    const QImage::Format fmt = deep ? (alpha ? QImage::Format_RGBA64 : QImage::Format_RGBX64)
                                    : (alpha ? QImage::Format_RGBA8888
                                             : QImage::Format_RGBX8888);
    QImage img(w, h, fmt);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const double u = double(x) / (w - 1);
            const double v = double(y) / (h - 1);
            if (deep) {
                auto* p = reinterpret_cast<uint16_t*>(img.scanLine(y)) + size_t(x) * 4;
                p[0] = uint16_t(u * 65535.0);
                p[1] = uint16_t(v * 65535.0);
                p[2] = uint16_t((1.0 - u) * 65535.0);
                p[3] = alpha ? uint16_t(u * 65535.0) : 65535;
            } else {
                auto* p = img.scanLine(y) + size_t(x) * 4;
                p[0] = uint8_t(u * 255.0);
                p[1] = uint8_t(v * 255.0);
                p[2] = uint8_t((1.0 - u) * 255.0);
                p[3] = alpha ? uint8_t(u * 255.0) : 255;
            }
        }
    }
    return img;
}

QImage decodeBack(const QByteArray& jxl) {
    // Читаем СВОИМ читателем: плагинов у нас больше нет вовсе.
    return decodeImage(jxl);
}

void checkRoundtrip() {
    EncodeMeta meta;
    QString err;

    // --- lossless обязан быть без потерь. Это не тавтология: если забыть
    // uses_original_profile, libjxl переведёт во внутреннее XYB, и «без
    // потерь» потеряет.
    {
        const QImage src = gradient(64, 48, false, false);
        EncodeOptions opt;
        opt.lossless = true;
        const QByteArray jxl = encodeJxl(src, opt, meta, &err);
        ZT_TRUE("lossless закодировался: " + err.toStdString(), !jxl.isEmpty());
        const QImage back = decodeBack(jxl);
        ZT_TRUE("lossless прочитался", !back.isNull());
        if (!back.isNull()) {
            ZT_EQ("размеры те же", num(src.width()) + "x" + num(src.height()),
                  num(back.width()) + "x" + num(back.height()));
            int diff = 0;
            const QImage a = src.convertToFormat(QImage::Format_RGB888);
            const QImage b = back.convertToFormat(QImage::Format_RGB888);
            for (int y = 0; y < a.height() && diff == 0; ++y)
                if (std::memcmp(a.constScanLine(y), b.constScanLine(y), size_t(a.width()) * 3))
                    diff = y + 1;
            ZT_EQ("lossless без единого отличия (строка расхождения)", num(0), num(diff));
        }
    }

    // --- глубина источника сохраняется -----------------------------------
    {
        const QImage src = gradient(64, 48, true, false);
        EncodeOptions opt;
        opt.lossless = true;
        const QByteArray jxl = encodeJxl(src, opt, meta, &err);
        ZT_TRUE("глубокая закодировалась", !jxl.isEmpty());
        const QImage back = decodeBack(jxl);
        ZT_TRUE("глубокая вернулась глубокой (" + num(back.depth()) + " бит/пиксель)",
                back.depth() == 64);
    }

    // --- потолок разрядности режет, но не ниже ---------------------------
    {
        const QImage src = gradient(32, 32, true, false);
        EncodeOptions opt;
        opt.lossless = true;
        opt.maxBitsPerChannel = 12;
        const QByteArray jxl = encodeJxl(src, opt, meta, &err);
        ZT_TRUE("с потолком 12 закодировалась", !jxl.isEmpty());
        const QImage back = decodeBack(jxl);
        // Двенадцать бит Qt всё равно отдаёт шестнадцатибитным форматом: своего
        // двенадцатибитного у него нет. Важно, что глубина НЕ упала до восьми.
        ZT_TRUE("двенадцать бит не выродились в восемь", back.depth() == 64);
    }

    // --- альфа переживает оба режима -------------------------------------
    for (bool lossless : {true, false}) {
        const QImage src = gradient(48, 48, false, true);
        EncodeOptions opt;
        opt.lossless = lossless;
        const QByteArray jxl = encodeJxl(src, opt, meta, &err);
        const QImage back = decodeBack(jxl);
        const std::string mode = lossless ? " (lossless)" : " (lossy)";
        ZT_TRUE("с альфой закодировалось" + mode, !jxl.isEmpty());
        ZT_TRUE("альфа дожила" + mode, !back.isNull() && back.hasAlphaChannel());
    }
}

void checkColorSpace() {
    EncodeMeta meta;
    QString err;

    // Пространство источника обязано доехать. Треть снимков владельца — P3, и
    // подмена его на sRGB — потеря необратимая.
    for (auto named : {QColorSpace::DisplayP3, QColorSpace::AdobeRgb, QColorSpace::SRgb}) {
        QImage src = gradient(32, 32, false, false);
        src.setColorSpace(QColorSpace(named));
        const std::string what = " (" + QColorSpace(named).description().toStdString() + ")";

        EncodeOptions opt;
        opt.lossless = true;
        const QByteArray jxl = encodeJxl(src, opt, meta, &err);
        ZT_TRUE("закодировалось" + what, !jxl.isEmpty());
        const QImage back = decodeBack(jxl);
        ZT_TRUE("профиль вернулся" + what, !back.isNull() && back.colorSpace().isValid());
        if (back.colorSpace().isValid()) {
            // Сравниваем не имена (их пишут по-разному), а сами первичные
            // цвета: пространство должно совпасть по существу.
            ZT_TRUE("пространство то же по существу" + what,
                    back.colorSpace() == QColorSpace(named));
        }
    }
}

void checkMetadata() {
    QString err;
    QImage src = gradient(32, 32, false, false);

    // Блоб EXIF собираем руками: важно, что он доедет и разберётся обратно.
    EncodeMeta meta;
    meta.exif = QByteArray("II", 2);
    meta.exif += QByteArray("\x2a\x00", 2);
    meta.exif += QByteArray("\x08\x00\x00\x00", 4);
    meta.exif += QByteArray("\x01\x00", 2);              // одна запись
    meta.exif += QByteArray("\x12\x01", 2);              // тег 274, Orientation
    meta.exif += QByteArray("\x03\x00", 2);              // SHORT
    meta.exif += QByteArray("\x01\x00\x00\x00", 4);
    meta.exif += QByteArray("\x06\x00", 2);              // значение 6
    meta.exif += QByteArray("\x00\x00", 2);
    meta.exif += QByteArray("\x00\x00\x00\x00", 4);
    meta.xmp = QByteArray("<x:xmpmeta xmlns:x='adobe:ns:meta/'></x:xmpmeta>");

    ZT_TRUE("собранный вручную блоб — настоящий EXIF",
            looksLikeExif(std::string_view(meta.exif.constData(), size_t(meta.exif.size()))));

    EncodeOptions opt;
    opt.lossless = true;
    const QByteArray jxl = encodeJxl(src, opt, meta, &err);
    ZT_TRUE("с метаданными закодировалось: " + err.toStdString(), !jxl.isEmpty());

    // Боксы делают файл контейнером, а не голым потоком: это видно по подписи.
    ZT_TRUE("файл стал контейнером",
            jxl.size() > 12 && std::memcmp(jxl.constData() + 4, "JXL ", 4) == 0);
    // И он всё ещё читается.
    ZT_TRUE("с боксами картинка читается", !decodeBack(jxl).isNull());

    // Без метаданных контейнер не нужен — проверяем, что мы его не навязываем.
    const QByteArray bare = encodeJxl(src, opt, EncodeMeta{}, &err);
    ZT_TRUE("без метаданных тоже кодируется", !bare.isEmpty());
    ZT_TRUE("и он меньше, чем с боксами", bare.size() < jxl.size());
}

void checkTranscode(const QString& root) {
    QString err;
    struct Case { const char* path; const char* why; };
    const Case cases[] = {
        {"art/leonardo-oldmen.jpg", "обычный baseline"},
        {"art/leonardo-tiny.jpg", "крошечный"},
        {"photo/progressive.jpg", "ПРОГРЕССИВНЫЙ"},
        {"photo/phone-gps.jpg", "с толстым EXIF и GPS"},
        {"photo/orientation6.jpg", "с поворотом"},
        {"deep/jpegli-adobergb.jpeg", "кодирован jpegli, не libjpeg"},
    };
    for (const Case& c : cases) {
        const QString p = root + "/" + c.path;
        if (!QFileInfo::exists(p)) continue;
        QFile f(p);
        if (!f.open(QIODevice::ReadOnly)) continue;
        const QByteArray jpeg = f.readAll();
        f.close();

        const std::string what = std::string(c.why) + " — ";
        const QByteArray jxl = transcodeJpegToJxl(jpeg, &err);
        ZT_TRUE(what + "транскод удался: " + err.toStdString(), !jxl.isEmpty());
        if (jxl.isEmpty()) continue;

        // ИНВАРИАНТ A этапа 8: транскод принимается только после обратной
        // сборки, совпавшей с исходником. Сравниваем сами байты — это строже
        // любого отпечатка.
        const QByteArray back = reconstructJpeg(jxl, &err);
        ZT_TRUE(what + "собрался обратно: " + err.toStdString(), !back.isEmpty());
        ZT_TRUE(what + "БАЙТ В БАЙТ", back == jpeg);
        ZT_TRUE(what + "и вышло не больше исходного", jxl.size() < jpeg.size());
    }

    // Не-JPEG транскодироваться не должен, и молчать об этом нельзя.
    const QString png = root + "/quality/png-прозрачность.png";
    if (QFileInfo::exists(png)) {
        QFile f(png);
        if (!f.open(QIODevice::ReadOnly)) return;
        err.clear();
        const QByteArray jxl = transcodeJpegToJxl(f.readAll(), &err);
        ZT_TRUE("PNG не транскодируется", jxl.isEmpty());
        ZT_TRUE("и причина названа", !err.isEmpty());
    }

    // Сборка JPEG из обычного JXL невозможна — данных для неё там нет.
    {
        const QImage src = gradient(32, 32, false, false);
        EncodeOptions opt;
        const QByteArray plain = encodeJxl(src, opt, EncodeMeta{}, &err);
        err.clear();
        ZT_TRUE("из обычного JXLJPEG не собирается", reconstructJpeg(plain, &err).isEmpty());
        ZT_TRUE("и об этом сказано", !err.isEmpty());
    }
}

}  // namespace

static int ztRunSuite(int argc, char** argv) {
    checkRoundtrip();
    checkColorSpace();
    checkMetadata();
    if (argc > 1) checkTranscode(QString::fromLocal8Bit(argv[1]));
    return zt::report("запись JXL");
}

// Набор целиком одним TEST: тело не тронуто, argv ему собран здесь.
// Дробить на отдельные проверки — отдельная работа, по одному набору.
TEST(JxlEncode, All) {
    std::vector<QByteArray> ztArgs{QByteArrayLiteral("jxl_encode_test")};
    ztArgs.push_back((zt::TestData::corpus(QStringLiteral("images/originals"))).toLocal8Bit());
    std::vector<char*> ztArgv;
    for (QByteArray& a : ztArgs) ztArgv.push_back(a.data());
    EXPECT_EQ(0, ztRunSuite(int(ztArgv.size()), ztArgv.data()));
}
