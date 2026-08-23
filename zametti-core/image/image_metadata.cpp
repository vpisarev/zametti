#include "image_metadata.h"

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

ImageMetadata ImageMetadata::fromFile(const QString& absolutePath) {
    ImageMetadata out;
    const QFileInfo file(absolutePath);
    out.name_ = file.fileName();
    out.valid_ = true;
    if (!file.exists()) return out;   // рамка вместо фотографии: сказать об этом надо

    out.exists_ = true;
    out.bytes_ = file.size();
    // Только шапка: пикселей здесь не надо, а место под фотографию считается
    // по НАСТОЯЩИМ размерам.
    out.probe_ = probeImageFile(absolutePath);
    // Формат опознаётся по подписи, но у чужих его может не оказаться вовсе.
    // Расширение — запасной ход, а не первый: имя файла врёт легко, заголовок
    // не врёт.
    if (out.probe_.format.isEmpty()) out.probe_.format = file.suffix().toLower();
    out.taken_ = shotTime(absolutePath);
    return out;
}

void ImageMetadata::addDecoded(const QImage& image) {
    ImageMetadata& out = *this;
    if (image.isNull()) return;
    // Глубина — по представлению разжатой копии, а не по имени формата:
    // шестнадцатибитный файл Qt отдаёт в шестнадцатибитном представлении, и
    // это ровно то, с чем работает показ.
    switch (image.format()) {
        case QImage::Format_RGBX64:
        case QImage::Format_RGBA64:
        case QImage::Format_RGBA64_Premultiplied:
        case QImage::Format_Grayscale16:
            out.bits_ = 16;
            break;
        case QImage::Format_RGBX16FPx4:
        case QImage::Format_RGBA16FPx4:
        case QImage::Format_RGBA16FPx4_Premultiplied:
            out.bits_ = 16;   // с плавающей точкой, но бит на канал столько же
            break;
        default:
            out.bits_ = 8;
            break;
    }
    const QColorSpace space = image.colorSpace();
    if (!space.isValid()) {
        // Профиля в файле нет вовсе (обычное дело у webp и png). Это не
        // «неизвестно»: и мы, и любой просмотрщик читаем такие отсчёты как
        // sRGB — так и пишем, потому что именно так они и показаны.
        out.colorSpace_ = QStringLiteral("sRGB");
        return;
    }
    out.colorSpace_ = space.description();
    // Профиль без имени — обычное дело у файлов из камер. Тогда называем то,
    // что знаем наверняка: основные цвета.
    if (!out.colorSpace_.isEmpty()) return;
    switch (space.primaries()) {
        case QColorSpace::Primaries::SRgb: out.colorSpace_ = QStringLiteral("sRGB"); break;
        case QColorSpace::Primaries::DciP3D65: out.colorSpace_ = QStringLiteral("Display P3"); break;
        case QColorSpace::Primaries::AdobeRgb: out.colorSpace_ = QStringLiteral("Adobe RGB"); break;
        case QColorSpace::Primaries::ProPhotoRgb:
            out.colorSpace_ = QStringLiteral("ProPhoto RGB");
            break;
        default: break;
    }
}

}  // namespace zametti
