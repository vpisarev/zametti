// Что известно про вложенную картинку, не разжимая её.
//
// Всё берётся из ЗАГОЛОВКА файла и из самой заметки. Пикселей не трогаем: под
// кареткой картинка меняется на каждое движение, а разжать снимок с телефона —
// это десятки миллисекунд и десятки мегабайт.
//
// Имя исходника здесь не читается из XMP, хотя мы его туда и пишем
// (xmpMM:PreservedFileName): в заметке оно уже есть — это подпись картинки,
// alt. Лезть за ним в файл значило бы читать бокс JXL на каждое движение
// каретки ради того, что лежит в трёх сантиметрах выше.

#ifndef ZAMETTI_IMAGE_METADATA_H
#define ZAMETTI_IMAGE_METADATA_H

#include "image_read.h"

#include <QDateTime>
#include <QSize>

class QImage;
#include <QString>

namespace zametti {

class ImageMetadata {
public:
    ImageMetadata() = default;

    // Всё, что видно из ЗАГОЛОВКА файла: имя, формат, размеры, вес, число
    // кадров, время съёмки. Файла нет — exists() ложь, но valid() истина: про
    // картинку известно хотя бы то, что её нет.
    static ImageMetadata fromFile(const QString& absolutePath);
    // Досыпать то, что видно только у РАЗЖАТОЙ копии: цветовое пространство и
    // глубину. Зовётся там, где картинку и так разжали для показа.
    void addDecoded(const QImage& image);

    const QString& name() const { return name_; }          // имя файла: в хранилище — id
    // Подпись из заметки — там и живёт имя исходника; ставит вид (у файла её нет).
    const QString& caption() const { return caption_; }
    void setCaption(const QString& caption) { caption_ = caption; }
    // Всё, что читатели знают о файле до разжатия (image_read.h), лежит одной
    // структурой probe_, а не копией полей: заголовок читается один раз, здесь.
    const QString& format() const { return probe_.format; }   // "jxl", "jpeg"…; пусто — чужой
    QSize size() const { return probe_.size; }                // из заголовка, настоящие пиксели
    int frames() const { return probe_.frames; }              // больше одного — анимация
    // У уже разжатой копии спрашиваем цвет и глубину. Разжимать ради панели
    // нельзя: снимок с телефона это десятки миллисекунд и десятки мегабайт.
    const QString& colorSpace() const { return colorSpace_; }
    int bits() const { return bits_; }                     // бит на канал; 0 — не спрашивали
    qint64 bytes() const { return bytes_; }
    // Когда снимок СДЕЛАН — из EXIF (DateTimeOriginal), а если его нет, из XMP
    // (xmp:CreateDate): тот же порядок источников, что у ввоза, и другого быть
    // не должно — иначе одна и та же картинка получала бы две разные даты.
    // Зоны в EXIF нет вовсе: время местное, каким его записала камера.
    // Недействительная дата — не ошибка: у снимка может не быть ни EXIF, ни XMP.
    QDateTime taken() const { return taken_; }
    bool exists() const { return exists_; }   // файла нет: не приехало или удалено руками
    bool valid() const { return valid_; }     // каретка не на картинке — показывать нечего

    // Сборка по частям — тому, кто знает о картинке не из файла (наборы,
    // сведения из заметки). Обычный путь — fromFile.
    void setName(const QString& v) { name_ = v; }
    void setFormat(const QString& v) { probe_.format = v; }
    void setSize(QSize v) { probe_.size = v; }
    void setBytes(qint64 v) { bytes_ = v; }
    void setTaken(const QDateTime& v) { taken_ = v; }
    void setExists(bool v) { exists_ = v; }
    void setValid(bool v) { valid_ = v; }

    bool operator==(const ImageMetadata& other) const = default;

protected:
    QString name_;
    QString caption_;
    ImageProbe probe_;
    QString colorSpace_;
    int bits_ = 0;
    qint64 bytes_ = 0;
    QDateTime taken_;
    bool exists_ = false;
    bool valid_ = false;
};

}  // namespace zametti

#endif  // ZAMETTI_IMAGE_METADATA_H
