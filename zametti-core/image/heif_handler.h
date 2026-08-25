// Чтение AVIF и HEIC через системную libheif.
//
// Один обработчик на оба формата: внутри у них общий контейнер ISOBMFF, и
// libheif различает их сама. Плагина под Qt6 в дистрибутиве нет вовсе, а шесть
// файлов матрицы перекодирования — как раз avif и heic.
//
// AVIF для нас формат ВХОДА, не хранения: пришёл — перевели в JXL. Поэтому
// вендорить libheif со всеми её видеокодеками мы не стали, хватает системной.
//
// Обязанности те же, что у читателя JXL: донести глубину (в корпусе владельца
// есть и 10-, и 12-битные avif), цветовое пространство (все 14 heic — Display
// P3) и поворот (все 14 heic повёрнуты).
//
// ПОЧЕМУ ЭТО QImageIOHandler, А НЕ ПАРА probe/decode, КАК У СОСЕДЕЙ. Плагином
// Qt он давно не является — зовётся напрямую из image_read, минуя реестр. Форма
// осталась от той поры; разбирать её ради единообразия не стали: он написан и
// работает.

#pragma once

#include <QColorSpace>
#include <QImageIOHandler>
#include <QSize>

struct heif_context;

namespace zametti {

class HeifHandler : public QImageIOHandler {
public:
    HeifHandler();
    ~HeifHandler() override;

    // ЗАРЕГИСТРИРОВАТЬ НАШИ ДЕКОДЕРЫ В libheif. Зовётся ОДИН РАЗ из точки
    // входа процесса — и обязательно ДО того, как заведён хоть один рабочий
    // поток: реестр плагинов libheif это обычный std::set без замка, а
    // картинки у нас читаются и из фонового потока ввоза, и из нескольких
    // потоков сразу, когда libheif разбирает плиточную сетку.
    //
    // Возвращает true, если зарегистрировали именно мы; false — libheif уже
    // знала декодер AV1, то есть кто-то позвал раньше. Своего флага здесь
    // нет: состояние принадлежит libheif, у неё и спрашиваем.
    static bool registerCodecs();

    static bool peek(QIODevice* device);

    bool canRead() const override;
    bool read(QImage* image) override;

    QVariant option(ImageOption option) const override;
    void setOption(ImageOption option, const QVariant& value) override;
    bool supportsOption(ImageOption option) const override;

private:
    bool readHeader() const;

    mutable QByteArray data_;
    mutable heif_context* ctx_ = nullptr;
    mutable bool haveHeader_ = false;
    mutable bool headerFailed_ = false;
    mutable QSize size_;
    mutable int bitsPerPixel_ = 8;
    mutable bool hasAlpha_ = false;
    mutable QByteArray icc_;   // вложенный профиль, если он есть
    mutable QColorSpace named_;  // иначе — названное пространство из nclx
    bool scanned_ = false;
};

}  // namespace zametti
