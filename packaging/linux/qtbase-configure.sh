#!/bin/bash
#
# Configure static Qt (qtbase) against the Ubuntu 20.04 sysroot.
#
#   git -C $ZBUILD clone --depth 1 --branch v6.10.3 https://code.qt.io/qt/qtbase.git
#   bash packaging/linux/qtbase-configure.sh
#   cd $ZBUILD/qtbase-build && cmake --build . -j8 && cmake --install .
#
# Kept as a file so it can be repeated and read rather than reconstructed from
# memory. CONFIGURES but does not build: the Qt build is long, and its exit
# code must be seen on its own: `cmake --build . | tail` returns $? from
# `tail`, so a failed build reports zero (been bitten by that).
#
# How to read the switch list:
#   -system-  the library is both in the sysroot and on every target machine;
#   -qt-      a private copy is cheaper than one more dynamic dependency;
#   -no-      the thing drags in the world (glib, ICU) or is not needed at all
#             (CUPS: we print via our own PDF output).
set -e

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=zenv.sh
source "$HERE/zenv.sh"

[ -d "$ZBUILD/qtbase" ] || { echo "no qtbase tree at $ZBUILD/qtbase" >&2; exit 1; }
mkdir -p "$ZBUILD/qtbase-build"
cd "$ZBUILD/qtbase-build"

"$ZBUILD/qtbase/configure" \
    -prefix "$ZPREFIX" \
    -static -release \
    -opensource -confirm-license \
    -nomake examples -nomake tests \
    -openssl-linked \
    -xcb -fontconfig \
    -system-freetype -system-zlib -system-libpng \
    -qt-pcre -qt-harfbuzz -qt-doubleconversion -qt-libjpeg \
    -no-glib -no-icu -no-cups \
    -- \
    -DCMAKE_TOOLCHAIN_FILE="$HERE/toolchains/qt-zsys.cmake" \
    -DOPENSSL_ROOT_DIR="$ZPREFIX" \
    -DOPENSSL_USE_STATIC_LIBS=ON \
    -DWaylandScanner_EXECUTABLE="$ZSYS/usr/bin/wayland-scanner"
