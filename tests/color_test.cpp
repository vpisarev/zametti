// Приведение цвета: тот единственный признак, по которому картинка переживает
// круг записи и чтения.
//
// Проверка тут не «функция что-то вернула», а САМ ИНВАРИАНТ: записали —
// прочитали — получили те же цвета. Именно его нарушение стоило нам оценки
// −40 на 12% выборки, причём одинаковой при любом качестве.
//
// Каждая проверка идёт парой: сперва показывается, что БЕЗ приведения круг не
// сходится, потом — что с приведением сходится. Зелёный тест ничего не значит,
// пока не показано, что он краснеет без починки.

#include "color.h"
#include "tiff_reader.h"
#include "jxl_encoder.h"

#include "test_util.h"

#include <QBuffer>
#include <QColorSpace>
#include <QGuiApplication>
#include <QImage>
#include <QFileInfo>
#include <QImageReader>

#include <array>
#include <cmath>
#include <string>

using namespace zametti;

namespace {

std::string num(double v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.2f", v);
    return buf;
}

// Средние по каналам — грубая, но достаточная мера «те же цвета или нет».
// Расхождение цветового пространства сдвигает их сразу и заметно: на снимке
// iPhone было 52/49/42 против 28/23/15.
std::array<double, 3> mean(const QImage& src) {
    const QImage img = src.convertToFormat(QImage::Format_RGBX8888);
    std::array<double, 3> out{0, 0, 0};
    for (int y = 0; y < img.height(); ++y) {
        const uchar* row = img.constScanLine(y);
        for (int x = 0; x < img.width(); ++x)
            for (int c = 0; c < 3; ++c) out[size_t(c)] += row[size_t(x) * 4 + size_t(c)];
    }
    const double n = double(img.width()) * img.height();
    for (double& v : out) v /= n;
    return out;
}

double maxDelta(const std::array<double, 3>& a, const std::array<double, 3>& b) {
    double d = 0;
    for (size_t i = 0; i < 3; ++i) d = std::max(d, std::fabs(a[i] - b[i]));
    return d;
}

QImage roundTrip(const QImage& src) {
    EncodeOptions opt;
    opt.quality = 90;
    opt.effort = 5;
    QString err;
    const QByteArray jxl = encodeJxl(src, opt, EncodeMeta{}, &err);
    if (jxl.isEmpty()) return {};
    QBuffer buf;
    buf.setData(jxl);
    buf.open(QIODevice::ReadOnly);
    QImageReader reader(&buf, "jxl");
    return reader.read();
}

// Картинка с цветом во всех углах: на однотонной заплате расхождение
// пространств можно и не заметить.
QImage colorful(int w, int h) {
    QImage img(w, h, QImage::Format_RGBX8888);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            uchar* p = img.scanLine(y) + size_t(x) * 4;
            p[0] = uchar(40 + 200 * x / w);
            p[1] = uchar(30 + 200 * y / h);
            p[2] = uchar(200 - 150 * x / w);
            p[3] = 255;
        }
    return img;
}

void checkCanonicalRecognition() {
    ZT_TRUE("sRGB выражается описанием",
            iccIsCanonical(QColorSpace(QColorSpace::SRgb).iccProfile()));
    ZT_TRUE("Display P3 выражается описанием",
            iccIsCanonical(QColorSpace(QColorSpace::DisplayP3).iccProfile()));
    ZT_TRUE("Adobe RGB выражается описанием",
            iccIsCanonical(QColorSpace(QColorSpace::AdobeRgb).iccProfile()));
    ZT_TRUE("пустой профиль — не канонический", !iccIsCanonical(QByteArray()));
    ZT_TRUE("мусор вместо профиля — не канонический",
            !iccIsCanonical(QByteArray("это не ICC, а просто байты")));
    ZT_TRUE("цель перевода сама канонична", iccIsCanonical(displayP3Icc()));
}

void checkCanonicalUntouched() {
    // Каноническое пространство приводить не к чему — функция обязана
    // ОТКАЗАТЬСЯ и не тронуть ни пикселя. Иначе мы бы гоняли лишний перевод на
    // каждой второй картинке.
    QImage img = colorful(64, 48);
    img.setColorSpace(QColorSpace(QColorSpace::DisplayP3));
    const QImage before = img;
    QByteArray icc;
    ZT_TRUE("канонический профиль не приводится", !canonicalizeColor(img, icc));
    ZT_TRUE("пиксели не тронуты", img == before);
    ZT_TRUE("профиль не подменён", icc.isEmpty());

    QImage plain = colorful(32, 32);   // вовсе без профиля
    QByteArray none;
    ZT_TRUE("без профиля приводить нечего", !canonicalizeColor(plain, none));
}

void checkProfileMustMatchPixels(const QString& root) {
    // ПРОФИЛЬ, ОПИСЫВАЮЩИЙ НЕ ТЕ ПИКСЕЛИ, обязан быть выброшен, а не применён.
    //
    // Откуда берётся такая пара. У CMYK-JPEG от Adobe в файле лежит печатный
    // CMYK-профиль, а Qt отдаёт нам пиксели уже переведёнными в RGB своими
    // силами. Профиль и пиксели после этого про разное, и перевод «по профилю»
    // даёт правдоподобную чепуху: замерено — средний RGB 41/41/38 там, где
    // правда 99/97/78.
    //
    // Профиль печати берём настоящий, из файла корпуса: сочинить CMYK-профиль
    // средствами Qt нельзя, а на выдуманном проверка ничего бы не стоила.
    const QString path = root + QStringLiteral("/museum/cmyk.tif");
    if (root.isEmpty() || !QFileInfo::exists(path)) return;
    TiffImage tiff;
    QString err;
    if (!readTiff(path, &tiff, &err) || tiff.icc.isEmpty()) {
        ZT_TRUE("CMYK-профиль для проверки достался: " + err.toStdString(), false);
        return;
    }
    ZT_TRUE("профиль печати не считается описывающим RGB", !iccDescribesRgb(tiff.icc));
    ZT_TRUE("обычный профиль считается описывающим RGB",
            iccDescribesRgb(QColorSpace(QColorSpace::DisplayP3).iccProfile()));

    QImage rgb = colorful(64, 48);
    const QImage before = rgb;
    QByteArray icc = tiff.icc;
    ZT_TRUE("по чужому профилю не переводим", !canonicalizeColor(rgb, icc));
    ZT_TRUE("пиксели не тронуты", rgb == before);
    // Выбросить надо совсем: класть в хранилище профиль, который не описывает
    // пиксели, — то же враньё, только отложенное до чтения.
    ZT_TRUE("и сам профиль выброшен", icc.isEmpty());
    ZT_TRUE("пометка с картинки снята", !rgb.colorSpace().isValid());
}

void checkConvertIsRealTransform() {
    // Перевод в то же самое пространство обязан быть тождественным с точностью
    // до округления. Если движок собран неправильно, это вылезет сразу.
    QImage same = colorful(64, 48);
    const auto before = mean(same);
    ZT_TRUE("sRGB → sRGB собирается",
            convertIcc(same, QColorSpace(QColorSpace::SRgb).iccProfile(),
                       QColorSpace(QColorSpace::SRgb).iccProfile()));
    ZT_TRUE("sRGB → sRGB ничего не меняет (сдвиг " + num(maxDelta(before, mean(same))) + ")",
            maxDelta(before, mean(same)) <= 1.0);

    // А вот перевод в заметно другое пространство обязан пиксели ИЗМЕНИТЬ:
    // иначе «перевод удался» означало бы «перевод не случился».
    QImage other = colorful(64, 48);
    ZT_TRUE("sRGB → ProPhoto собирается",
            convertIcc(other, QColorSpace(QColorSpace::SRgb).iccProfile(),
                       QColorSpace(QColorSpace::ProPhotoRgb).iccProfile()));
    ZT_TRUE("другое пространство даёт другие числа (сдвиг " +
                num(maxDelta(before, mean(other))) + ")",
            maxDelta(before, mean(other)) > 3.0);

    ZT_TRUE("пустой профиль — отказ, а не тихий пропуск",
            !convertIcc(other, QByteArray(), QColorSpace(QColorSpace::SRgb).iccProfile()));

    // Шестнадцатибитный путь: та же проверка на глубоком формате. У него своя
    // ветка чтения строки, и она обязана быть проверена отдельно.
    QImage deep = colorful(48, 32).convertToFormat(QImage::Format_RGBX64);
    const auto deepBefore = mean(deep);
    ZT_TRUE("16 бит: перевод собирается",
            convertIcc(deep, QColorSpace(QColorSpace::SRgb).iccProfile(),
                       QColorSpace(QColorSpace::SRgb).iccProfile()));
    ZT_TRUE("16 бит: глубина сохранена", deep.depth() > 32);
    ZT_TRUE("16 бит: тождественный перевод ничего не меняет",
            maxDelta(deepBefore, mean(deep)) <= 1.0);
}

// Главная проверка. Настоящий снимок с табличным профилем Apple: без
// приведения круг не сходится, с приведением сходится.
void checkRoundTripClosesOnWideGamut(const QString& root) {
    if (root.isEmpty()) return;
    const QString path = root + QStringLiteral("/iphone/hdr.jpg");
    QImageReader reader(path);
    reader.setAutoTransform(true);
    const QImage src = reader.read();
    if (src.isNull()) return;   // корпуса нет — эту проверку пропускаем

    ZT_TRUE("у снимка iPhone профиль, не выражаемый описанием",
            !iccIsCanonical(src.colorSpace().iccProfile()));

    // СНЯТИЕ ПОЧИНКИ: как было до неё. Круг обязан НЕ сойтись — иначе проверка
    // ниже ничего не доказывает.
    const QImage backRaw = roundTrip(src);
    ZT_TRUE("без приведения картинка всё же читается", !backRaw.isNull());
    const double broken = backRaw.isNull() ? 0.0 : maxDelta(mean(src), mean(backRaw));
    ZT_TRUE("без приведения цвета уезжают (сдвиг " + num(broken) + " из 255)", broken > 10.0);

    // А теперь как есть.
    QImage fixed = src;
    QByteArray icc;
    ZT_TRUE("снимок приводится к каноническому пространству", canonicalizeColor(fixed, icc));
    ZT_TRUE("после приведения профиль канонический", iccIsCanonical(icc));
    ZT_TRUE("после приведения пространство — Display P3",
            fixed.colorSpace() == QColorSpace(QColorSpace::DisplayP3));

    const QImage back = roundTrip(fixed);
    ZT_TRUE("приведённая картинка читается обратно", !back.isNull());
    if (back.isNull()) return;
    const double delta = maxDelta(mean(fixed), mean(back));
    ZT_TRUE("круг сходится по цвету (сдвиг " + num(delta) + " из 255)", delta <= 2.0);
    ZT_TRUE("прочитанное — тоже Display P3",
            back.colorSpace() == QColorSpace(QColorSpace::DisplayP3));
}

// Канонический широкий охват обязан проходить круг и БЕЗ всякого приведения:
// ради этого вся затея и была.
void checkCanonicalWideGamutSurvives() {
    QImage img = colorful(256, 192);
    img.setColorSpace(QColorSpace(QColorSpace::DisplayP3));
    const QImage back = roundTrip(img);
    ZT_TRUE("Display P3 читается обратно", !back.isNull());
    if (back.isNull()) return;
    // Допуск 4, а не 2, как на настоящем снимке: у синтетического градиента
    // нет ни шума, ни мелких деталей, и XYB на q90 сдвигает его среднее на
    // 2.57 (замерено). Сигнал же от расхождения пространств — 24 и больше,
    // так что запас шестикратный и проверка своё ловит.
    ZT_TRUE("Display P3 переживает круг (сдвиг " + num(maxDelta(mean(img), mean(back))) + ")",
            maxDelta(mean(img), mean(back)) <= 4.0);
}

}  // namespace

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    QImageReader::setAllocationLimit(2048);
    const QString root = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QString();
    checkCanonicalRecognition();
    checkCanonicalUntouched();
    checkProfileMustMatchPixels(root);
    checkConvertIsRealTransform();
    checkCanonicalWideGamutSurvives();
    checkRoundTripClosesOnWideGamut(root);
    return zt::report("цвет");
}
