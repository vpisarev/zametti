#!/usr/bin/env bash
# Configure static Qt 6.10.3 for Windows x86-64 (mingw-w64), cross-built from Linux.
# It only CONFIGURES -- build and install separately, so that the exit code is
# read from the build itself and not from a pipeline (we have already been
# bitten by this, see the Linux recipe).
#
#   source packaging/win/zenv.sh
#   bash packaging/win/qtbase-configure.sh
#   cmake --build $ZWBUILD/qtbase-win -j8 && cmake --install $ZWBUILD/qtbase-win
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
: "${ZWPREFIX:?source packaging/win/zenv.sh}"
: "${ZWBUILD:?source packaging/win/zenv.sh}"
: "${QT_HOST_PATH:?source packaging/win/zenv.sh}"

SRC="${QTBASE_SRC:-$ZWBUILD/qtbase}"
BUILD="${QTBASE_WIN_BUILD:-$ZWBUILD/qtbase-win}"

[ -d "$SRC" ] || { echo "no qtbase sources in $SRC" >&2; exit 1; }

mkdir -p "$BUILD"
cd "$BUILD"

# Reads the same way as the Linux list, except that there is not a single
# "system-" here: the target machine has none of our stuff at all, and Windows
# ships no zlib/png/freetype of its own. So everything that can be is "qt-"
# (own copy inside), and everything that would pull in the outside world is
# "no-".
#
# -schannel instead of -openssl: TLS is provided by Windows itself, and its
# trusted-root store is its own, live and updated. This is exactly the reason
# we did the opposite on Linux -- linked OpenSSL and kept --openssldir=/etc/ssl:
# the goal is the same, trust the roots of the TARGET machine, not our own.
#
# -no-dbus: there is no bus on Windows; on Linux it was needed only by the
# adwaita plugin.
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
