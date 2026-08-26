#!/bin/bash
#
# Настройка статического Qt (qtbase) против sysroot Ubuntu 20.04.
#
#   git -C $ZBUILD clone --depth 1 --branch v6.10.3 https://code.qt.io/qt/qtbase.git
#   bash packaging/linux/qtbase-configure.sh
#   cd $ZBUILD/qtbase-build && cmake --build . -j8 && cmake --install .
#
# Записана файлом, чтобы её можно было повторить и прочитать, а не
# восстанавливать из памяти. НАСТРАИВАЕТ, но не собирает: сборка Qt долгая, и
# её код возврата надо видеть отдельно — `cmake --build . | tail` отдаёт $? от
# `tail`, и упавшая сборка отрапортует нулём (наступали).
#
# Как читать список ключей:
#   -system-  библиотека есть и в sysroot, и на любой целевой машине;
#   -qt-      своя копия дешевле лишней динамической зависимости;
#   -no-      вещь тянет за собой мир (glib, ICU) или не нужна вовсе (CUPS —
#             на бумагу мы выводим своим PDF).
set -e

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=zenv.sh
source "$HERE/zenv.sh"

[ -d "$ZBUILD/qtbase" ] || { echo "нет дерева qtbase в $ZBUILD/qtbase" >&2; exit 1; }
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
