#!/usr/bin/env bash
# Настройка статической Qt 6.10.3 под Windows x86-64 (mingw-w64), кросс с Linux.
# Только НАСТРАИВАЕТ — собирать и ставить отдельно, чтобы код возврата сборки
# читался у сборки, а не у конвейера (на этом уже наступали, см. рецепт Linux).
#
#   source packaging/win/zenv.sh
#   bash packaging/win/qtbase-configure.sh
#   cmake --build $ZWBUILD/qtbase-win -j4 && cmake --install $ZWBUILD/qtbase-win
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
: "${ZWPREFIX:?source packaging/win/zenv.sh}"
: "${ZWBUILD:?source packaging/win/zenv.sh}"
: "${QT_HOST_PATH:?source packaging/win/zenv.sh}"

SRC="${QTBASE_SRC:-$ZWBUILD/qtbase}"
BUILD="${QTBASE_WIN_BUILD:-$ZWBUILD/qtbase-win}"

[ -d "$SRC" ] || { echo "нет исходников qtbase в $SRC" >&2; exit 1; }

mkdir -p "$BUILD"
cd "$BUILD"

# Читается так же, как список под Linux, только «system-» здесь нет ни одного:
# на целевой машине нашего добра нет вовсе, а Windows своего zlib/png/freetype
# не даёт. Поэтому всё, что можно, — «qt-» (своя копия внутри), а всё, что
# тянет мир, — «no-».
#
# -schannel вместо -openssl: TLS даёт сама Windows, и хранилище доверенных
# корней у неё своё, живое и обновляемое. Это ровно та причина, по которой под
# Linux мы, наоборот, слинковали OpenSSL и оставили --openssldir=/etc/ssl —
# цель одна: доверять корням ЦЕЛЕВОЙ машины, а не своим.
#
# -no-dbus: под Windows шины нет; на Linux он нужен был только плагину adwaita.
"$SRC/configure" \
    -prefix "$ZWPREFIX" \
    -static -release \
    -opensource -confirm-license \
    -nomake examples -nomake tests \
    -qt-host-path "$QT_HOST_PATH" \
    -schannel \
    -qt-zlib -qt-libpng -qt-freetype -qt-harfbuzz \
    -qt-pcre -qt-doubleconversion -qt-libjpeg \
    -no-icu -no-dbus -no-glib \
    -opengl desktop \
    -- \
    -DCMAKE_TOOLCHAIN_FILE="$HERE/toolchains/mingw-w64.cmake"
