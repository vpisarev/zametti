#!/bin/bash
#
# Переносимая сборка zametti под macOS: один файл, внутри — статический Qt из
# $ZPREFIX и всё вендоренное добро (libjxl, libheif, libsodium, zstd, blake3,
# microtex…). Снаружи остаются ТОЛЬКО системные рамки, которые есть на любом
# маке по построению и вкладывать которые нельзя.
#
#   bash packaging/mac/qtbase-configure.sh
#   cd $ZBUILD/qtbase-build && cmake --build . -j8 && cmake --install .
#   bash packaging/mac/build-qtsvg.sh
#   bash packaging/mac/build-zametti.sh
#
# Собирается ключами САМОГО Qt (qt-cmake), а не своими: разошедшиеся флаги у Qt
# и у программы — классический способ получить бинарь, который собрался, но не
# запускается. Ровно тем же приёмом собран qtsvg.
set -e

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
# shellcheck source=zenv.sh
source "$HERE/zenv.sh"

[ -x "$ZPREFIX/bin/qt-cmake" ] || {
    echo "нет $ZPREFIX/bin/qt-cmake — сперва соберите Qt:" >&2
    echo "        bash packaging/mac/qtbase-configure.sh" >&2
    exit 1
}

OUT="${OUT:-$ROOT/build-portable}"
"$ZPREFIX/bin/qt-cmake" -S "$ROOT" -B "$OUT" \
    -DCMAKE_BUILD_TYPE=Release \
    -DWITH_STATIC_QT=ON \
    -DCMAKE_OSX_DEPLOYMENT_TARGET="$ZMACOS_MIN" \
    -DCMAKE_OSX_ARCHITECTURES="$ZARCHS"
cmake --build "$OUT" -j8 --target zametti

echo
echo "=== что получилось ==="
ls -lh "$OUT/app/zametti" | awk '{print "  размер:", $5}'
echo "  чужого снаружи (не системные рамки):"
otool -L "$OUT/app/zametti" | tail -n +2 | grep -v -E "/usr/lib/|/System/Library/" | sed 's/^/    /' || true
echo "  (пусто выше = ни одной чужой библиотеки)"
