#!/bin/bash
#
# Обновление вендоренного libheif. Запускать так:
#
#   git clone --depth 1 --branch v1.23.2 \
#       https://github.com/strukturag/libheif /куда-нибудь
#   3rdparty/libheif/update.sh /куда-нибудь 3rdparty/libheif/upstream
#
# ЗАЧЕМ ОН НАМ. Это разбор контейнера ISOBMFF, в котором лежат HEIC и AVIF:
# сетки плиток, повороты, альфа отдельным item'ом, ICC и nclx. Сама она не
# декодирует ничего — кодеки зовёт со стороны. Мы даём ей libde265 (HEVC) и
# СВОЙ плагин на libgav1 (AV1), см. zametti-core/image/.
#
# ТОЛЬКО ЧТЕНИЕ. Ни одного энкодера: пишем мы один формат, JPEG XL.
#
# ЧТО БЕРЁТСЯ: CMakeLists.txt, COPYING, libheif.pc.in, cmake/, libheif/, gnome/
# (три безобидные строки под if(TARGET heif-thumbnailer)).
#
# ЧТО ВЫБРАСЫВАЕТСЯ: examples/ (1.2 МБ вместе с example.heic), tests/ (2.2 МБ,
# из них 0.9 МБ — амальгама catch2), fuzzing/, scripts/, go/, gdk-pixbuf/,
# logos/, third-party/ (там только .cmd-скрипты сборки чужих деревьев),
# extra/ (getopt для примеров под MSVC), проза и настройки чужого CI.
#
# И САМОЕ ВАЖНОЕ — ЗАГЛУШКА ВМЕСТО heifio/CMakeLists.txt.
#
# `add_subdirectory(heifio)` стоит в корне чужой сборки БЕЗУСЛОВНО (строка
# 597), опции для него нет. А настоящий heifio/CMakeLists.txt делает
# find_package на TIFF, JPEG, PNG, WEBP и ZLIB — то есть привёл бы в нашу
# сборку ВТОРУЮ libtiff рядом с нашей вендоренной, ВТОРУЮ libwebp рядом с
# нашей, и системный zlib мимо 3rdparty/zlib. Ни одна цель heifio нам не
# нужна: это входные форматы для heif-enc, которого мы не собираем.
#
# Поэтому каталог остаётся, а CMakeLists.txt в нём — пустой. Пустым обязан,
# существовать — тоже обязан. Тот же приём, что у 3rdparty/libjxl/update.sh.
set -e
SRC="$1"
DST="$2"
[ -d "$SRC" ] || { echo "нет исходного дерева: $SRC" >&2; exit 1; }
[ -n "$DST" ] || { echo "не сказано, куда класть" >&2; exit 1; }

rm -rf "$DST"
mkdir -p "$DST"
DST="$(cd "$DST" && pwd)"

cd "$SRC"
tar -cf - CMakeLists.txt COPYING libheif.pc.in cmake libheif gnome | (cd "$DST" && tar -xf -)

cd "$DST"
find . -name '.git*' -prune -exec rm -rf {} + 2>/dev/null || true

mkdir -p heifio
cat > heifio/CMakeLists.txt <<'STUB'
# ЗАГЛУШКА, ПОСТАВЛЕННАЯ НАМИ. Настоящего heifio в вендоренном дереве нет.
#
# Корневой CMakeLists.txt libheif делает add_subdirectory(heifio) безусловно,
# без всякой опции, — поэтому файл обязан существовать. А настоящий heifio
# зовёт find_package на TIFF, JPEG, PNG, WEBP и ZLIB и притащил бы в сборку
# вторые копии библиотек, которые у нас уже вендорены. Его цели — входные
# форматы для утилиты heif-enc, которую мы не собираем.
#
# Ставится скриптом update.sh; правки здесь переживут только до следующего
# обновления.
STUB

echo "libheif: $(du -sh . | cut -f1), файлов $(find . -type f | wc -l)"
