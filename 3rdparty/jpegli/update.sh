#!/bin/bash
#
# Обновление вендоренного jpegli. Запускать так:
#
#   git clone https://github.com/google/jpegli.git /куда-нибудь/jpegli
#   3rdparty/jpegli/update.sh /куда-нибудь/jpegli 3rdparty/jpegli/upstream
#
# ПОДМОДУЛИ НЕ НУЖНЫ НИ ОДИН. Highway у нас общая (3rdparty/highway), skcms
# нужна не декодеру, а другим целям jpegli, которых мы не собираем — проверено:
# в libjpegli-static.a ноль символов skcms. Из libjpeg-turbo берутся ТРИ
# ЗАГОЛОВКА, и те лежат в самом дереве после
#
#   git submodule update --init --depth 1 third_party/libjpeg-turbo
#
# У google/jpegli РЕЛИЗОВ НЕТ ВОВСЕ — ни одного тега. Поэтому закрепляем
# коммит и пишем его в CMakeLists рядом; при обновлении переписать там же.
#
# Своя сборка, а не чужая (см. CMakeLists.txt рядом): их CMake завёл бы вторую
# цель skcms поверх той, что уже есть у libjxl, и вторую highway.

set -e
SRC="$1"
DST="$2"
[ -d "$SRC" ] || { echo "нет исходного дерева: $SRC" >&2; exit 1; }
[ -n "$DST" ] || { echo "не сказано, куда класть" >&2; exit 1; }

rm -rf "$DST"
mkdir -p "$DST"
# Дальше будет cd в исходник, после которого относительный путь назначения
# указывал бы не туда.
DST="$(cd "$DST" && pwd)"

cd "$SRC"
tar -cf - \
  LICENSE PATENTS AUTHORS CONTRIBUTORS \
  lib/jpegli lib/base lib/jpegli_lists.cmake \
  third_party/libjpeg-turbo/jconfig.h.in \
  third_party/libjpeg-turbo/jpeglib.h \
  third_party/libjpeg-turbo/jmorecfg.h \
  third_party/libjpeg-turbo/jerror.h \
| (cd "$DST" && tar -xf -)

cd "$DST"
find . -name '.git*' -prune -exec rm -rf {} + 2>/dev/null || true
# Тесты и стенды: к декоду отношения не имеют.
find . \( -name '*_test.cc' -o -name '*_test.h' -o -name '*_testlib*' \
       -o -name '*gbench*' -o -name 'test_utils*' \) -delete 2>/dev/null || true

echo "jpegli: $(du -sh . | cut -f1), файлов $(find . -type f | wc -l)"
