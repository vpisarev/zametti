#!/bin/sh
# zametti_lossless.png правится руками, zametti1024x1024.png собирается отсюда.
#
# Имя несёт размер: иконка своя на каждую систему, и 512-я (zametti512x512.png)
# лежит рядом отдельным файлом. Псевдоним в qrc при этом нейтральный.
#
# Шаг pngquant — не украшение: без него в бинарник уходит 255 207 байт вместо
# 98 158 при PSNR 46.54 дБ и SSIM(8x8) 0.9965 по ДОМНОЖЕННОМУ на альфу цвету.
# Разницы не видно и на увеличении втрое. Меры по сырому RGBA тут врут: 16%
# точек полностью прозрачны, и палитра пишет под ними что угодно.
#
# С 05.09.2026 тем же шагом собирается и alt-редакция: исходники
# zametti_alt_lossless_1024.png / _512.png (RGBA, правятся руками), выход —
# zametti_alt_1024x1024.png / zametti_alt_512x512.png, на которые смотрят qrc,
# CMake и build-app.sh. Меры alt-пары — в шапке resources.qrc.
set -e
squeeze() {
    optipng -quiet -o7 -strip all "$1"
    pngquant 256 --speed 1 --force --output __zametti__.png "$1"
    zopflipng -y -m __zametti__.png "$2"
    rm -f __zametti__.png
}
squeeze zametti_lossless.png zametti1024x1024.png
squeeze zametti_alt_lossless_1024.png zametti_alt_1024x1024.png
squeeze zametti_alt_lossless_512.png zametti_alt_512x512.png
