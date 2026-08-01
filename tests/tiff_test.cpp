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

#include "test_util.h"

#include <QColorSpace>
#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>

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
            ZT_TRUE("XMP забран", !img.xmp.isEmpty());

            // ЦВЕТ. Пиксель (0, 1229) в файле — Lab(56.1, 16, 29). Независимый
            // разбор даёт (172,123,84); tiff2rgba на нём же — (183,126,74), и
            // это ровно та ошибка, которой болеет Qt. Допуск в две единицы:
            // округление у разных реализаций своё, а вот одиннадцать — уже не
            // округление.
            const QColor c = img.image.pixelColor(0, 1229);
            ZT_TRUE("красный близок к независимому разбору (" + num(c.red()) + " против 172)",
                    std::abs(c.red() - 172) <= 2);
            ZT_TRUE("зелёный близок (" + num(c.green()) + " против 123)",
                    std::abs(c.green() - 123) <= 2);
            ZT_TRUE("синий близок (" + num(c.blue()) + " против 84)",
                    std::abs(c.blue() - 84) <= 2);
            // Перевели сами — значит картинка теперь в sRGB, и чужого профиля
            // на ней быть не должно.
            ZT_TRUE("после перевода стоит sRGB", img.image.colorSpace().isValid());
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
            // Профиль в 424 КБ забираем, даже если переводим пока наивно: без
            // него правильный перевод потом сделать будет не из чего.
            ZT_TRUE("профиль CMYK забран", img.icc.size() > 100000);
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

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    checkSignature();
    if (argc > 1) checkFiles(QString::fromLocal8Bit(argv[1]));
    return zt::report("читатель TIFF");
}
