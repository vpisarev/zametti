#include "image_facts.h"

#include "exif.h"
#include "image_read.h"

#include <QFile>

#include <QFileInfo>
#include <QColorSpace>
#include <QImage>

namespace zametti {
namespace {

// Когда снимок сделан. Порядок источников тот же, что у ввоза (image_insert.cpp,
// shotSeconds): EXIF DateTimeOriginal — момент съёмки, XMP xmp:CreateDate —
// сканы и экспорт из редакторов. Дальше ввоз идёт к датам файла, а мы НЕ идём:
// в панели сказано «создана», и подсунуть туда время копирования файла значило
// бы соврать. Нет метаданных — молчим.
//
// Читаем НЕ ВЕСЬ файл: метаданные лежат в начале (у нашего JXL боксы Exif и
// "xml " идут перед кодовым потоком), а снимок бывает и на сорок мегабайт.
// Мегабайта хватает с запасом; не хватило — блоб не разберётся, и мы просто
// ничего не покажем.
QDateTime shotTime(const QString& absolutePath) {
    QFile file(absolutePath);
    if (!file.open(QIODevice::ReadOnly)) return {};
    const QByteArray head = file.read(1024 * 1024);
    const ImageMeta meta =
        readImageMeta(std::string_view(head.constData(), size_t(head.size())));

    QString when = QString::fromStdString(exifDateTaken(meta.exif));
    if (when.isEmpty()) when = QString::fromStdString(xmpCreateDate(meta.xmp));
    const QDateTime taken = QDateTime::fromString(when, Qt::ISODate);
    if (!taken.isValid()) return {};
    // Зоны в EXIF нет — время местное. Собираем его заново из даты и времени,
    // чтобы Qt не сочла отсутствие зоны за UTC и не сдвинула час.
    return QDateTime(taken.date(), taken.time());
}

}  // namespace

void readImageFacts(const QString& absolutePath, ImageFacts& out) {
    out.path = absolutePath;
    const QFileInfo file(absolutePath);
    out.name = file.fileName();
    out.valid = true;
    if (!file.exists()) return;   // рамка вместо фотографии: сказать об этом надо

    out.exists = true;
    out.bytes = file.size();
    // Только шапка: пикселей здесь не надо, а место под фотографию считается
    // по НАСТОЯЩИМ размерам.
    const ImageProbe probe = probeImageFile(absolutePath);
    out.size = probe.size;
    out.format = probe.format;
    // Формат опознаётся по подписи, но у чужих его может не оказаться вовсе.
    // Расширение — запасной ход, а не первый: имя файла врёт легко, заголовок
    // не врёт.
    if (out.format.isEmpty()) out.format = file.suffix().toLower();
    out.frames = probe.frames;
    out.taken = shotTime(absolutePath);
}

void addDecodedFacts(const QImage& image, ImageFacts& out) {
    if (image.isNull()) return;
    // Глубина — по представлению разжатой копии, а не по имени формата:
    // шестнадцатибитный файл Qt отдаёт в шестнадцатибитном представлении, и
    // это ровно то, с чем работает показ.
    switch (image.format()) {
        case QImage::Format_RGBX64:
        case QImage::Format_RGBA64:
        case QImage::Format_RGBA64_Premultiplied:
        case QImage::Format_Grayscale16:
            out.bits = 16;
            break;
        case QImage::Format_RGBX16FPx4:
        case QImage::Format_RGBA16FPx4:
        case QImage::Format_RGBA16FPx4_Premultiplied:
            out.bits = 16;   // с плавающей точкой, но бит на канал столько же
            break;
        default:
            out.bits = 8;
            break;
    }
    const QColorSpace space = image.colorSpace();
    if (!space.isValid()) {
        // Профиля в файле нет вовсе (обычное дело у webp и png). Это не
        // «неизвестно»: и мы, и любой просмотрщик читаем такие отсчёты как
        // sRGB — так и пишем, потому что именно так они и показаны.
        out.colorSpace = QStringLiteral("sRGB");
        return;
    }
    out.colorSpace = space.description();
    // Профиль без имени — обычное дело у файлов из камер. Тогда называем то,
    // что знаем наверняка: основные цвета.
    if (!out.colorSpace.isEmpty()) return;
    switch (space.primaries()) {
        case QColorSpace::Primaries::SRgb: out.colorSpace = QStringLiteral("sRGB"); break;
        case QColorSpace::Primaries::DciP3D65: out.colorSpace = QStringLiteral("Display P3"); break;
        case QColorSpace::Primaries::AdobeRgb: out.colorSpace = QStringLiteral("Adobe RGB"); break;
        case QColorSpace::Primaries::ProPhotoRgb:
            out.colorSpace = QStringLiteral("ProPhoto RGB");
            break;
        default: break;
    }
}

}  // namespace zametti
