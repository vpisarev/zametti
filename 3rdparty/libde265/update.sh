#!/bin/bash
#
# Обновление вендоренного libde265. Запускать так:
#
#   git clone --depth 1 --branch v1.1.1 \
#       https://github.com/strukturag/libde265 /куда-нибудь
#   3rdparty/libde265/update.sh /куда-нибудь 3rdparty/libde265/upstream
#
# ЗАЧЕМ ОН НУЖЕН. Это декодер HEVC — то, чем разжимается HEIC. libheif сама
# ничего не декодирует, она разбирает контейнер и зовёт кодек; без de265 из неё
# получается библиотека, которая честно отказывает всем heic подряд.
#
# ПОЧЕМУ ЧУЖОЙ CMake, А НЕ СВОЙ. У upstream он есть, работает и сам раздаёт
# пофайловые ключи SIMD (libde265/x86/*.cc собираются с -msse4.1, -mavx2,
# -mavx512f по отдельности). Свой список пришлось бы держать в согласии с
# чужим при каждом обновлении — ровно то, от чего уводит комментарий в
# 3rdparty/libwebp/CMakeLists.txt.
#
# АССЕМБЛЕРА НА НАШИХ ПЛАТФОРМАХ У НЕГО НЕТ. Четыре файла .S лежат в
# libde265/arm32/ и компилируются только под тридцатидвухбитный ARM (чужой
# CMake прячет их за HAVE_ARM32). Мы их ОСТАВЛЯЕМ: 56 КБ, чужой код, и
# выбрасывать поддержку платформы ради этих килобайт — не обрезка, а решение,
# которого никто не принимал.
#
# ЧТО БЕРЁТСЯ:
#   CMakeLists.txt   — корень чужой сборки;
#   COPYING          — LGPL-3, обязателен;
#   libde265.pc.in   — читается чужим CMake через configure_file;
#   cmake/           — там config.h.in, без него падает configure_file;
#   libde265/        — сама библиотека;
#   extra/           — win32cond.c, который libde265/CMakeLists.txt подключает
#                      под MSVC/MINGW, и каталог которого прописан в
#                      include_directories корня. Выглядит лишним на Linux, но
#                      без него ломается сборка под Windows, а туда мы идём.
#
# ЧТО ВЫБРАСЫВАЕТСЯ: dec265/ и sherlock265/ (утилита командной строки и
# Qt-просмотрщик — чужие программы, нам не нужна ни одна), fuzzing/, проза
# (README, NEWS, ChangeLog, AUTHORS, SECURITY.md, .png) и CMakePresets.json.
#
# ОСТОРОЖНО, ИМЯ ВРЁТ: у чужого CMake есть опция ENABLE_DECODER, и она включает
# НЕ декодер, а утилиту dec265. Сама библиотека собирается безусловно. Мы её
# гасим — см. комментарий в CMakeLists.txt рядом.
set -e
SRC="$1"
DST="$2"
[ -d "$SRC" ] || { echo "нет исходного дерева: $SRC" >&2; exit 1; }
[ -n "$DST" ] || { echo "не сказано, куда класть" >&2; exit 1; }

rm -rf "$DST"
mkdir -p "$DST"
DST="$(cd "$DST" && pwd)"

cd "$SRC"
tar -cf - CMakeLists.txt COPYING libde265.pc.in cmake libde265 extra | (cd "$DST" && tar -xf -)

cd "$DST"
find . -name '.git*' -prune -exec rm -rf {} + 2>/dev/null || true

echo "libde265: $(du -sh . | cut -f1), файлов $(find . -type f | wc -l)"
