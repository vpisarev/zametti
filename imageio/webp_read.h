// Чтение WebP через вендоренную libwebp.
//
// Только чтение: пишем мы ровно один формат — JPEG XL. Читать надо потому, что
// в хранилище лежат webp-вложения (следы разового ввоза чужого дерева), и чужой
// webp человек вправе принести со стороны.
//
// ICC, EXIF и XMP отсюда не достаются: их берёт свой обход чанков RIFF
// (readWebpMeta в exif.h). Демуксер libwebp нам поэтому не нужен, и мы его не
// собираем вовсе.
//
// Анимацию не крутим: заметке нужен кадр, а не кино. У анимированного webp
// берётся то, что отдаёт простой декодер, — первый кадр.

#pragma once

#include <QByteArray>
#include <QImage>
#include <QSize>

namespace zametti {

// Размеры по заголовку, без разжатия. Пусто — не webp или заголовок битый.
QSize readWebpSize(const QByteArray& bytes);

// Разжатие целиком. Уменьшать по дороге libwebp не умеет — у неё нет
// многоуровневого представления, как восьмые доли у JPEG; значит уменьшает
// вызывающий, уже после.
QImage decodeWebp(const QByteArray& bytes);

// Опознать по подписи контейнера RIFF: "RIFF" .... "WEBP".
bool looksLikeWebp(const QByteArray& head);

}  // namespace zametti
