#!/bin/bash
#
# Обновление вендоренного libjxl. Запускать так:
#
#   git clone --depth 1 --branch v0.12.0 --recurse-submodules --shallow-submodules \
#       https://github.com/libjxl/libjxl.git /куда-нибудь/libjxl
#   3rdparty/libjxl/update.sh /куда-нибудь/libjxl 3rdparty/libjxl/upstream
#
# Подмодули нужны не все: хватает highway, brotli и skcms. Остальные
# (googletest, lcms, libjpeg-turbo, libpng, sjpeg, zlib, HEVCSoftware) питают
# lib/extras и тесты, а мы ни того, ни другого не собираем — входные форматы
# декодирует Qt. Их можно не вытягивать вовсе:
#
#   git submodule update --init --depth 1 \
#       third_party/highway third_party/brotli third_party/skcms
#
# После обновления обязательно: сверить, что собранные библиотеки совпадают с
# теми, что даёт ПОЛНОЕ дерево со всеми подмодулями. Именно этой сверкой
# проверено, что выборка ниже ничего нужного не выбрасывает.
#
# Сверху остаётся version в 3rdparty/libjxl/CMakeLists.txt — правится руками,
# в двух местах (JXL_VENDORED_VERSION и JPEGXL_VERSION).

set -e
SRC="$1"
DST="$2"
[ -d "$SRC" ] || { echo "нет исходного дерева: $SRC" >&2; exit 1; }
[ -n "$DST" ] || { echo "не сказано, куда класть" >&2; exit 1; }

rm -rf "$DST"
mkdir -p "$DST"

# Что берём. tools/ — целиком (1.8 МБ), ради ssimulacra2 и gauss_blur: дешевле
# сохранить их CMake нетронутым, чем править его под себя. Файлы .in и
# cmake_uninstall.cmake.in выглядят лишними, но без них чужой CMake падает на
# configure_file — трогать не надо.
cd "$SRC"
tar -cf - \
  CMakeLists.txt cmake_uninstall.cmake.in \
  LICENSE PATENTS AUTHORS CONTRIBUTORS \
  cmake lib tools \
  third_party/CMakeLists.txt third_party/dirent.cc third_party/dirent.h \
  third_party/skcms.cmake third_party/lcms2.cmake third_party/sjpeg.cmake third_party/testing.cmake \
  third_party/highway third_party/brotli third_party/skcms \
| (cd "$DST" && tar -xf -)

cd "$DST"

# Гит-метаданные, чужие CI, документация и корпуса тестовых данных: к сборке
# отношения не имеют, а весят больше самого кода. highway/docs выброшен ещё и
# потому, что тамошний Makefile попадал под наш .gitignore — дерево в
# репозитории обязано быть ровно тем, что даёт этот скрипт.
find . -name '.git*' -prune -exec rm -rf {} + 2>/dev/null || true
rm -rf \
  third_party/brotli/{tests,java,go,python,csharp,js,docs,research} \
  third_party/highway/{g3doc,docs} \
  third_party/skcms/{bazel,profiles,fuzz} \
  2>/dev/null || true

echo "libjxl: $(du -sh . | cut -f1), файлов $(find . -type f | wc -l)"
