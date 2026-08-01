// Чтение JPEG XL через вендоренную libjxl.
//
// Зачем свой обработчик, а не дистрибутивный плагин: JXL — основной формат
// хранения картинок zametti, и декод обязан быть одинаковым на всех машинах.
// Дистрибутивный плагин где-то есть, где-то нет, где-то другой версии.
//
// Что обработчик обязан донести до Qt, помимо пикселей:
//
//   * ГЛУБИНУ. Источник в 10 или 12 бит не имеет права стать восьмибитным по
//     дороге — это была бы потеря, которой никто не просил. Всё, что глубже
//     восьми, читается в шестнадцатибитный формат Qt;
//   * ЦВЕТОВОЕ ПРОСТРАНСТВО. Профиль забирается из потока и ставится картинке.
//     Треть снимков в каталогах владельца — Display P3; молча объявить их sRGB
//     значит переврать цвета;
//   * ПОВОРОТ. JXL несёт ориентацию в заголовке. Отдаём её через
//     ImageTransformation, а поворачивает пусть Qt — так же, как для JPEG.

#pragma once

#include <QImageIOHandler>
#include <QSize>

namespace zametti {

class JxlHandler : public QImageIOHandler {
public:
    JxlHandler();
    ~JxlHandler() override;

    static bool peek(QIODevice* device);

    bool canRead() const override;
    bool read(QImage* image) override;

    QVariant option(ImageOption option) const override;
    void setOption(ImageOption option, const QVariant& value) override;
    bool supportsOption(ImageOption option) const override;

private:
    // Читает заголовок и запоминает всё, что о картинке известно ДО разжатия
    // пикселей: размер, глубину, наличие альфы, поворот, профиль. На этом
    // стоят границы входа — они обязаны срабатывать до декода.
    bool readHeader() const;

    mutable QByteArray data_;      // весь поток; libjxl удобнее один буфер
    QByteArray scratch_;           // на случай, если шаг строки Qt шире строки
    mutable bool haveHeader_ = false;
    mutable bool headerFailed_ = false;
    mutable QSize size_;
    mutable int bitsPerSample_ = 8;
    mutable bool hasAlpha_ = false;
    mutable QByteArray icc_;
    mutable Transformations transform_ = TransformationNone;
    bool scanned_ = false;         // кадр уже отдан: JXL-анимацию не крутим
};

}  // namespace zametti
