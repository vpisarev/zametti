#!/bin/bash
#
# Обновление вендоренного libtiff. Запускать так:
#
#   git clone --depth 1 --branch v4.7.0 https://gitlab.com/libtiff/libtiff.git /куда-нибудь
#   3rdparty/libtiff/update.sh /куда-нибудь 3rdparty/libtiff/upstream
#
# Берётся ядро и его сборочные файлы; выбрасываются утилиты, тесты, примеры и
# документация — они втрое тяжелее самого кода и нам не нужны ни одной строкой.
#
# CMake у libtiff берём ЧУЖОЙ, а не пишем свой: он порождает tif_config.h по
# проверкам платформы (какие заголовки есть, какие типы, какой порядок байт).
# Ради переносимости на Mac и Windows это ровно та работа, которую не хочется
# делать руками — и ровно тот случай, когда чужой CMake полезен.
#
# Внешние кодеки все выключены, кроме Deflate: он опирается на нашу же
# вендоренную zlib. LZW, PackBits, CCITT и «без сжатия» у libtiff внутренние.

set -e
SRC="$1"
DST="$2"
[ -d "$SRC" ] || { echo "нет исходного дерева: $SRC" >&2; exit 1; }
[ -n "$DST" ] || { echo "не сказано, куда класть" >&2; exit 1; }

rm -rf "$DST"
mkdir -p "$DST"
DST="$(cd "$DST" && pwd)"

cd "$SRC"
tar -cf - \
  CMakeLists.txt VERSION RELEASE-DATE LICENSE.md README.md configure.ac \
  libtiff-4.pc.in placeholder.h \
  cmake libtiff port build \
| (cd "$DST" && tar -xf -)

cd "$DST"
find . -name '.git*' -prune -exec rm -rf {} + 2>/dev/null || true

echo "libtiff: $(du -sh . | cut -f1), файлов $(find . -type f | wc -l)"
