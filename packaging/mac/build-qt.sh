#!/bin/bash
#
# Static Qt for macOS end to end: patches + configure + build + install of
# qtbase, then qtsvg — for each architecture in $ZARCHS in turn.
#
#   git -C $ZBUILD clone --depth 1 --branch v6.10.3 https://code.qt.io/qt/qtbase.git
#   git -C $ZBUILD clone --depth 1 --branch v6.10.3 https://code.qt.io/qt/qtsvg.git
#   bash packaging/mac/build-qt.sh
#
# Configure and build used to be separate files — a guard against a manual
# `cmake --build . | tail`, where the exit code comes from tail and a failed
# build reports zero. There are no pipes here, and set -e -o pipefail stops the
# script at the first non-zero step, so the split is no longer needed.
#
# The version is the same as in the portable Linux build — 6.10.3 (the owner's
# word): two different Qts on two systems would mean two different sets of
# layout bugs, and acceptance snapshots must be compared against one and the
# same thing.
#
# WHY qtsvg. The toolbar icons are Lucide, i.e. SVG. The program links Qt6::Svg
# as a MODULE rather than relying on the imageformats plugin: the plugin lives
# in an optional package, and on a machine without it the toolbar would be
# silently empty (the argument is recorded in app/CMakeLists.txt). In a static
# build the module simply goes inside the binary.
#
# How to read the configure flag list:
#   -qt-      our own copy is cheaper than one more dynamic dependency;
#   -no-      the thing drags in the world (glib, ICU) or is not needed at all
#             (CUPS — we print via our own PDF).
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=zenv.sh
source "$HERE/zenv.sh"

[ -d "$ZBUILD/qtbase" ] || { echo "no qtbase tree in $ZBUILD/qtbase (clone commands are in the script header)" >&2; exit 1; }
[ -d "$ZBUILD/qtsvg" ] || { echo "no qtsvg tree in $ZBUILD/qtsvg (clone commands are in the script header)" >&2; exit 1; }

# QT PATCHES. Each carries its explanation inside the patch itself; they must
# be carried over on every Qt version bump, checking whether upstream has fixed
# the issue. Applied idempotently (--check before --apply) and ONCE per tree:
# the source tree is shared, nothing in the patches is arch-dependent, and both
# architectures are built from it out-of-source.
for patch in "$HERE"/patches/qtbase-*.patch; do
    [ -e "$patch" ] || continue
    if git -C "$ZBUILD/qtbase" apply --check "$patch" 2>/dev/null; then
        git -C "$ZBUILD/qtbase" apply "$patch"
        echo "patch applied: $(basename "$patch")"
    else
        echo "patch already in place (or does not apply): $(basename "$patch")"
    fi
done

# One architecture per pass and one build at a time (the machine has 32 GB,
# CLAUDE.md rule): never run both halves in parallel.
for arch in $ZARCHS; do
    prefix="$ZDEPS/$arch"
    bdir="$ZBUILD/qtbase-build-$arch"
    echo "=== qtbase [$arch] → $prefix ==="

    # A foreign architecture is a cross-build for Qt, and it demands
    # QT_HOST_PATH: a ready native-arch Qt supplying the build tools (moc,
    # rcc…). This is BETTER than running the whole configure under Rosetta:
    # the compiler stays native and merely targets the foreign arch. Hence the
    # order in ZARCHS — native first, foreign second.
    hostpath=()
    if [ "$arch" != "$ZHOSTARCH" ]; then
        [ -x "$ZDEPS/$ZHOSTARCH/bin/qt-cmake" ] || {
            echo "cross-build [$arch] needs the native Qt in $ZDEPS/$ZHOSTARCH —" >&2
            echo "first: ZARCHS=$ZHOSTARCH bash packaging/mac/build-qt.sh" >&2
            exit 1
        }
        hostpath=("-DQT_HOST_PATH=$ZDEPS/$ZHOSTARCH")
    fi

    mkdir -p "$bdir"
    cd "$bdir"
    # ${hostpath[@]+...} instead of a bare expansion: the system bash on the
    # mac is 3.2, where an empty array under set -u counts as an unset variable.
    "$ZBUILD/qtbase/configure" \
        -prefix "$prefix" \
        -static -release \
        -opensource -confirm-license \
        -nomake examples -nomake tests \
        -qt-pcre -qt-harfbuzz -qt-doubleconversion -qt-libjpeg -qt-libpng -qt-zlib \
        -no-glib -no-icu -no-cups \
        -- \
        -DCMAKE_OSX_DEPLOYMENT_TARGET="$ZMACOS_MIN" \
        -DCMAKE_OSX_ARCHITECTURES="$arch" \
        ${hostpath[@]+"${hostpath[@]}"}
    cmake --build . -j8
    cmake --install .

    echo "=== qtsvg [$arch] ==="
    sdir="$ZBUILD/qtsvg-build-$arch"
    mkdir -p "$sdir"
    cd "$sdir"
    # qt-cmake supplies the flags from the prefix — neither -static, nor the
    # floor, nor the architecture need repeating here, and they have nowhere
    # to drift apart. QT_HOST_PATH is demanded again by the toolchain file when
    # cross-building — we pass the same one.
    "$prefix/bin/qt-cmake" "$ZBUILD/qtsvg" -DCMAKE_BUILD_TYPE=Release \
        ${hostpath[@]+"${hostpath[@]}"}
    cmake --build . -j8
    cmake --install .
done

echo "=== Qt ready: $(for a in $ZARCHS; do printf '%s ' "$ZDEPS/$a"; done) ==="
