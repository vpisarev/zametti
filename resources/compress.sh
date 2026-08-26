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
set -e
optipng -quiet -o7 -strip all zametti_lossless.png
pngquant 256 --speed 1 --force --output __zametti__.png zametti_lossless.png
zopflipng -y -m __zametti__.png zametti1024x1024.png
rm -f __zametti__.png
