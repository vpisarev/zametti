// Читатель TIFF: пространства, глубина, метаданные, границы.
//
// Главное здесь — не «прочиталось», а «прочиталось ВЕРНО». Qt тоже читает Lab
// и тоже не падает; он просто отдаёт неправильные цвета. Поэтому цвет
// сверяется с числами, полученными независимо (см. docs и комментарий в
// tiff_reader.cpp), а не с тем, что вернул наш же код.
//
// Каталог с картинками — первым аргументом; без него проверяются только те
// случаи, для которых файл сочиняется на месте.

#include "tiff_reader.h"

#include <tiffio.h>

#include "test_util.h"

#include <vector>
#include "testdata.h"

#include <QColorSpace>
#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

using namespace zametti;

namespace {

std::string num(long long v) { return std::to_string(v); }

void checkSignature() {
    ZT_TRUE("II опознан", looksLikeTiff(QByteArray("II\x2a\x00", 4)));
    ZT_TRUE("MM опознан", looksLikeTiff(QByteArray("MM\x00\x2a", 4)));
    ZT_TRUE("BigTIFF опознан", looksLikeTiff(QByteArray("II\x2b\x00", 4)));
    ZT_TRUE("JPEG не опознан", !looksLikeTiff(QByteArray("\xff\xd8\xff\xe0", 4)));
    ZT_TRUE("PNG не опознан", !looksLikeTiff(QByteArray("\x89PNG", 4)));
    ZT_TRUE("короткое не опознано", !looksLikeTiff(QByteArray("II", 2)));
}

// CMYK без профиля — запасной путь читателя. Файл под него сочиняем на месте:
// в корпусе оба CMYK с профилем, а ветка без профиля стала после починки
// именно запасной, и оставлять её без проверки нельзя.
QString makeCmykTiff(const QString& dir, const QString& name, const QByteArray& icc) {
    const QString path = dir + "/" + name;
    TIFF* t = TIFFOpen(path.toLocal8Bit().constData(), "w");
    if (!t) return {};
    if (!icc.isEmpty())
        TIFFSetField(t, TIFFTAG_ICCPROFILE, uint32_t(icc.size()), icc.constData());
    const uint32_t w = 4, h = 2;
    TIFFSetField(t, TIFFTAG_IMAGEWIDTH, w);
    TIFFSetField(t, TIFFTAG_IMAGELENGTH, h);
    TIFFSetField(t, TIFFTAG_SAMPLESPERPIXEL, uint16_t(4));
    TIFFSetField(t, TIFFTAG_BITSPERSAMPLE, uint16_t(8));
    TIFFSetField(t, TIFFTAG_PHOTOMETRIC, uint16_t(PHOTOMETRIC_SEPARATED));
    TIFFSetField(t, TIFFTAG_PLANARCONFIG, uint16_t(PLANARCONFIG_CONTIG));
    TIFFSetField(t, TIFFTAG_COMPRESSION, uint16_t(COMPRESSION_NONE));
    // Первый пиксель — тот же набор красок, что и в углу картины Шишкина.
    uint8_t row[4 * 4] = {170, 181, 219, 80, 0, 0, 0, 0, 0, 0, 0, 255, 128, 64, 32, 16};
    bool ok = true;
    for (uint32_t y = 0; y < h && ok; ++y) ok = TIFFWriteScanline(t, row, y, 0) == 1;
    TIFFClose(t);
    return ok ? path : QString();
}

void checkCmykWithoutProfile() {
    QTemporaryDir tmp;
    if (!tmp.isValid()) return;
    const QString path = makeCmykTiff(tmp.path(), QStringLiteral("cmyk-без-профиля.tif"), {});
    ZT_TRUE("CMYK без профиля сочинён", !path.isEmpty());
    if (path.isEmpty()) return;

    TiffImage img;
    QString err;
    ZT_TRUE("CMYK без профиля прочитан: " + err.toStdString(), readTiff(path, &img, &err));
    if (img.isNull()) return;
    ZT_TRUE("опознан как CMYK", img.color == TiffColor::Cmyk);
    ZT_TRUE("отмечено, что пространство переводили мы", img.converted);
    ZT_TRUE("профиля в файле нет", img.icc.isEmpty());

    // Значения — по формуле (1-C)*(1-K), посчитанной на бумаге:
    //   C=170/255=0.6667, K=80/255=0.3137 -> (1-0.6667)*(1-0.3137)=0.2288 -> 58
    //   M=181/255=0.7098                  -> (1-0.7098)*0.6863   =0.1992 -> 51
    //   Y=219/255=0.8588                  -> (1-0.8588)*0.6863   =0.0969 -> 25
    const QColor c = img.image.pixelColor(0, 0);
    ZT_EQ("наивный красный", num(58), num(c.red()));
    ZT_EQ("наивный зелёный", num(51), num(c.green()));
    ZT_EQ("наивный синий", num(25), num(c.blue()));
    // Бумага без краски обязана выйти белой и на запасном пути тоже.
    const QColor paper = img.image.pixelColor(1, 0);
    ZT_EQ("бумага белая", num(255), num(paper.red()));
    // Сплошной чёрный (K=255) — чёрным.
    const QColor black = img.image.pixelColor(2, 0);
    ZT_EQ("сплошной K чёрный", num(0), num(black.red()));
}

void checkCmykWithForeignProfile() {
    // CMYK, но профиль в файле — НЕ CMYK. Такая пара бессмысленна, и важно, что
    // именно происходит: перевод по профилю невозможен (skcms переворачивает
    // краску только для CMYK-профилей, и подставить сюда sRGB значило бы
    // разойтись с ним в соглашении), значит идём наивным путём — а раз так,
    // чужой профиль на получившиеся пиксели вешать НЕЛЬЗЯ.
    //
    // Проверка различающая: убери в читателе ветку `out->color == Cmyk`, и
    // картинка уедет в `else`, где на приближённые пиксели встанет sRGB.
    QTemporaryDir tmp;
    if (!tmp.isValid()) return;
    const QByteArray srgb = QColorSpace(QColorSpace::SRgb).iccProfile();
    const QString path = makeCmykTiff(tmp.path(), QStringLiteral("cmyk-чужой-профиль.tif"), srgb);
    ZT_TRUE("CMYK с чужим профилем сочинён", !path.isEmpty());
    if (path.isEmpty()) return;

    TiffImage img;
    QString err;
    ZT_TRUE("прочитан: " + err.toStdString(), readTiff(path, &img, &err));
    if (img.isNull()) return;
    ZT_TRUE("профиль из файла забран", !img.icc.isEmpty());
    ZT_TRUE("опознан как CMYK", img.color == TiffColor::Cmyk);
    ZT_TRUE("чужой профиль на пиксели НЕ поставлен", !img.image.colorSpace().isValid());
    // И пиксели именно наивные, а не переведённые по чужому профилю.
    const QColor c = img.image.pixelColor(0, 0);
    ZT_EQ("красный наивный", num(58), num(c.red()));
    ZT_EQ("зелёный наивный", num(51), num(c.green()));
    ZT_EQ("синий наивный", num(25), num(c.blue()));
}

void checkFiles(const QString& root) {
    // --- CIELab: главный случай, ради которого читатель и написан ---------
    const QString lab = root + "/museum/lab-lzw.tif";
    if (QFileInfo::exists(lab)) {
        TiffImage img;
        QString err;
        ZT_TRUE("Lab-TIFF прочитан: " + err.toStdString(), readTiff(lab, &img, &err));
        if (!img.isNull()) {
            ZT_EQ("размеры", num(3169) + "x" + num(2458),
                  num(img.image.width()) + "x" + num(img.image.height()));
            ZT_TRUE("опознан как Lab", img.color == TiffColor::Lab);
            ZT_TRUE("отмечено, что пространство переводили мы", img.converted);
            ZT_EQ("глубина файла", num(8), num(img.bitsPerSample));
            // РАБОЧИЙ КАДР ШИРЕ ВХОДА. Файл восьмибитный, но Lab покрывает
            // охват шире, чем восьмибитный RGB, и перевод нелинеен — он
            // растягивает тёмные тона, где полосы видны сразу. Округлять
            // результат до восьми бит значило бы потерять то, ради чего свой
            // читатель и затевался. До двенадцати ужмёт энкодер, один раз.
            ZT_TRUE("кадр шестнадцатибитный, хотя файл восьмибитный",
                    img.image.depth() > 32);
            ZT_TRUE("XMP забран", !img.xmp.isEmpty());

            // ЦВЕТ. Пиксель (0, 1229) в файле — Lab(56.1, 16, 29). Цель
            // перевода — Display P3 (Lab покрывает весь видимый охват, и sRGB
            // упирал бы насыщенные краски в границу). Независимый расчёт по
            // формулам — адаптация Брэдфорда D50→D65, затем XYZ→линейный P3,
            // затем кривая sRGB — даёт (165, 125, 90).
            //
            // Для сверки: тот же пиксель в sRGB был бы (172, 123, 85), а
            // tiff2rgba даёт (183, 126, 74) — ровно та ошибка, которой болеет
            // Qt. Допуск в две единицы: округление у разных реализаций своё, а
            // вот одиннадцать — уже не округление.
            const QColor c = img.image.pixelColor(0, 1229);
            ZT_TRUE("красный близок к независимому расчёту (" + num(c.red()) + " против 165)",
                    std::abs(c.red() - 165) <= 2);
            ZT_TRUE("зелёный близок (" + num(c.green()) + " против 125)",
                    std::abs(c.green() - 125) <= 2);
            ZT_TRUE("синий близок (" + num(c.blue()) + " против 90)",
                    std::abs(c.blue() - 90) <= 2);
            // Перевели сами — значит картинка теперь в Display P3, им и
            // помечена. Чужой профиль (он описывал Lab) с ней больше не связан.
            ZT_TRUE("после перевода стоит Display P3",
                    img.image.colorSpace() == QColorSpace(QColorSpace::DisplayP3));
        }
    }

    // --- 16 бит и широкий охват: пиксели НЕ трогаем -----------------------
    const QString deep = root + "/deep/tiff-16бит-p3.tif";
    if (QFileInfo::exists(deep)) {
        TiffImage img;
        QString err;
        ZT_TRUE("16-битный TIFF прочитан: " + err.toStdString(), readTiff(deep, &img, &err));
        if (!img.isNull()) {
            ZT_EQ("глубина сохранена", num(16), num(img.bitsPerSample));
            ZT_TRUE("формат Qt глубокий", img.image.depth() == 64);
            ZT_TRUE("опознан как RGB", img.color == TiffColor::Rgb);
            ZT_TRUE("пространство НЕ переводили", !img.converted);
            ZT_TRUE("профиль забран", !img.icc.isEmpty());
            ZT_TRUE("профиль поставлен картинке", img.image.colorSpace().isValid());
            ZT_EQ("профиль тот самый", std::string("Display P3"),
                  img.image.colorSpace().description().toStdString());
        }
    }

    // --- серый ------------------------------------------------------------
    const QString gray = root + "/quality/tiff-обычный.tiff";
    if (QFileInfo::exists(gray)) {
        TiffImage img;
        QString err;
        ZT_TRUE("серый TIFF прочитан", readTiff(gray, &img, &err));
        if (!img.isNull()) ZT_TRUE("опознан как серый", img.color == TiffColor::Gray);
    }

    // --- CMYK -------------------------------------------------------------
    const QString cmyk = root + "/museum/cmyk.tif";
    if (QFileInfo::exists(cmyk)) {
        TiffImage img;
        QString err;
        ZT_TRUE("CMYK прочитан", readTiff(cmyk, &img, &err));
        if (!img.isNull()) {
            ZT_TRUE("опознан как CMYK", img.color == TiffColor::Cmyk);
            // Профиль в 424 КБ — «3M Matchprint», типографский. Без него
            // правильный перевод сделать не из чего.
            ZT_TRUE("профиль CMYK забран", img.icc.size() > 100000);
            ZT_TRUE("отмечено, что пространство переводили мы", img.converted);
            ZT_TRUE("кадр шестнадцатибитный, хотя файл восьмибитный",
                    img.image.depth() > 32);
            ZT_TRUE("после перевода стоит Display P3",
                    img.image.colorSpace() == QColorSpace(QColorSpace::DisplayP3));

            // ЦВЕТ. Числа получены НЕ нашим кодом: littleCMS переведён тем же
            // профилем в тот самый Display P3, который отдаёт Qt, намерение
            // относительно-колориметрическое. Это ровно тот движок, которым
            // считают GIMP и macOS, — с ними владелец нас и сравнил.
            //
            // Ради чего проверка. Раньше CMYK переводился наивно, (1-C)*(1-K),
            // и «Девятый вал» Айвазовского в хранилище выходил заметно
            // насыщеннее и контрастнее оригинала. Наивная формула на этих же
            // пикселях даёт (58,51,25) вместо (84,73,50) и (16,20,15) вместо
            // (17,22,19) — то есть проверка краснеет от снятия починки на
            // первом же пикселе.
            //
            // Допуск в три единицы: у skcms и lcms своя решётка интерполяции
            // и своё округление, замеренное расхождение на всей картине —
            // 0.72 из 255 в среднем при наибольшем 4 (tests/cmyk_probe).
            struct Point { int x, y, r, g, b; };
            const Point pts[] = {{0, 0, 84, 73, 50},
                                 {100, 100, 70, 72, 56},
                                 {2834, 1920, 66, 69, 48},
                                 {5000, 3000, 79, 68, 45},
                                 {5667, 3839, 17, 22, 19}};
            for (const Point& p : pts) {
                const QColor c = img.image.pixelColor(p.x, p.y);
                const std::string where = "(" + num(p.x) + "," + num(p.y) + ")";
                ZT_TRUE("CMYK " + where + " красный " + num(c.red()) + " против " + num(p.r),
                        std::abs(c.red() - p.r) <= 3);
                ZT_TRUE("CMYK " + where + " зелёный " + num(c.green()) + " против " + num(p.g),
                        std::abs(c.green() - p.g) <= 3);
                ZT_TRUE("CMYK " + where + " синий " + num(c.blue()) + " против " + num(p.b),
                        std::abs(c.blue() - p.b) <= 3);
            }
        }
    }

    // --- граница разжатия -------------------------------------------------
    if (QFileInfo::exists(deep)) {
        TiffImage img;
        QString err;
        // 6000x4000 при 16 битах просят 183 МБ; ставим потолок вдвое ниже.
        ZT_TRUE("потолок разжатия сработал",
                !readTiff(deep, &img, &err, 90LL * 1024 * 1024));
        ZT_TRUE("и объяснил, почему: " + err.toStdString(), err.contains("потолок"));
        // Проверка обязана срабатывать ДО чтения строк, иначе она бесполезна:
        // память уже была бы занята.
        ZT_TRUE("картинка не создавалась", img.isNull());
    }

    // --- не TIFF ----------------------------------------------------------
    const QString jpeg = root + "/art/leonardo-oldmen.jpg";
    if (QFileInfo::exists(jpeg)) {
        TiffImage img;
        QString err;
        ZT_TRUE("JPEG не притворяется TIFF", !readTiff(jpeg, &img, &err));
        ZT_TRUE("и об этом сказано", !err.isEmpty());
    }
}

}  // namespace

static int ztRunSuite(int argc, char** argv) {
    checkSignature();
    checkCmykWithoutProfile();
    checkCmykWithForeignProfile();
    if (argc > 1) checkFiles(QString::fromLocal8Bit(argv[1]));
    return zt::report("читатель TIFF");
}

// Набор целиком одним TEST: тело не тронуто, argv ему собран здесь.
// Дробить на отдельные проверки — отдельная работа, по одному набору.
TEST(Tiff, All) {
    std::vector<QByteArray> ztArgs{QByteArrayLiteral("tiff_test")};
    ztArgs.push_back((zt::TestData::corpus(QStringLiteral("images/originals"))).toLocal8Bit());
    std::vector<char*> ztArgv;
    for (QByteArray& a : ztArgs) ztArgv.push_back(a.data());
    EXPECT_EQ(0, ztRunSuite(int(ztArgv.size()), ztArgv.data()));
}
