#!/bin/bash
#
# Portable zametti build for macOS from cmake to dmg — ONE dmg PER ARCHITECTURE
# (owner's decision, 04.09.2026): few intel macs are left, and a universal
# binary would carry 30 MB of someone else's code to every user.
#
#   bash packaging/mac/build-qt.sh          # once; takes hours
#   bash packaging/mac/build-app.sh [arm|intel|all]   # the program; default arm
#
# What comes out (the build-portable/ layout is the owner's word: builds in
# per-architecture subfolders, results in the root):
#
#   build-portable/<arch>/            cmake tree of that half
#   build-portable/<arch>/Zametti.app the program for that architecture, ad-hoc signed
#   build-portable/Zametti-<version>-arm.dmg
#   build-portable/Zametti-<version>-intel.dmg
#
# The version comes from project(... VERSION) in the root CMakeLists.txt: at
# configure time cmake writes it to generated/version.txt and generates
# generated/Info.plist from packaging/mac/Info.plist.in. There is no second
# version number in this script.
#
# Inside the binary: the static Qt from $ZDEPS/<arch> and all the vendored
# goods (libjxl, libheif, libsodium, zstd, blake3, microtex…); outside remain
# ONLY the system frameworks, which exist on every mac by construction and
# must not be bundled. Symbols are stripped (owner's decision, 04.09.2026):
# the symbol table of the arm64 slice weighed ~7 MB out of 35.
#
# Signing is ad-hoc (codesign -s -): without it arm macs will not launch the
# binary at all. It does NOT remove the Gatekeeper warning on SOMEONE ELSE'S
# mac — only Developer ID plus notarization do, which is separate work
# (docs/zametti-build-macos.md, the "Долги" (debts) section).
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
# shellcheck source=zenv.sh
source "$HERE/zenv.sh"

APP_NAME="Zametti"

usage() {
    echo "usage: bash packaging/mac/build-app.sh [arm|intel|all]   (default: arm)" >&2
    exit 2
}
[ $# -le 1 ] || usage
case "${1:-arm}" in
    arm)   ARCHS="arm64" ;;
    intel) ARCHS="x86_64" ;;
    all)   ARCHS="arm64 x86_64" ;;
    *)     usage ;;
esac
# The half's name in the dmg name is human, not machine: "arm" and "intel"
# make sense to someone picking a file to download; "arm64" and "x86_64" do not.
tag_of() {
    case "$1" in
        arm64)  echo arm ;;
        x86_64) echo intel ;;
    esac
}

UNI="$ROOT/build-portable"
mkdir -p "$UNI"
APP_VERSION=""

# ── Icon: the .icns is built on the spot from resources/zametti_alt_1024x1024.png ──
# Once per run, the same one for both halves. No binary icns lives in the
# repository. 1024 is exactly 512@2x.
ICNS="$UNI/zametti.icns"
make_icns() {
    local iconset="$UNI/zametti.iconset"
    local src="$ROOT/resources/zametti_alt_1024x1024.png"
    rm -rf "$iconset"
    mkdir -p "$iconset"
    local size double
    for size in 16 32 128 256 512; do
        sips -z "$size" "$size" "$src" --out "$iconset/icon_${size}x${size}.png" > /dev/null
        double=$((size * 2))
        sips -z "$double" "$double" "$src" --out "$iconset/icon_${size}x${size}@2x.png" > /dev/null
    done
    iconutil -c icns "$iconset" -o "$ICNS"
    rm -rf "$iconset"
}

# ── 1. Building one half ─────────────────────────────────────────────────────
# One architecture per configure and one build at a time — the reasons are in
# zenv.sh (SIMD of the vendored libraries) and CLAUDE.md (the machine has 32 GB).
build_arch() {
    local arch="$1"
    local prefix="$ZDEPS/$arch"
    local out="$UNI/$arch"
    [ -x "$prefix/bin/qt-cmake" ] || {
        echo "no $prefix/bin/qt-cmake — build Qt first:" >&2
        echo "        ZARCHS=$arch bash packaging/mac/build-qt.sh" >&2
        exit 1
    }
    echo "=== zametti [$arch] → $out ==="
    local extra=()
    if [ "$arch" != "$ZHOSTARCH" ]; then
        # On the foreign architecture cmake would leave the host arch in
        # CMAKE_SYSTEM_PROCESSOR, and libgav1/libsodium/highway/libjxl would
        # pick the wrong SIMD (see zenv.sh). We set only the processor, WITHOUT
        # CMAKE_SYSTEM_NAME: the name would switch on cmake's full cross mode
        # with its own demands.
        extra+=("-DCMAKE_SYSTEM_PROCESSOR=$arch")
        # The cross-built Qt wrote into its toolchain file a demand for
        # QT_HOST_PATH — the native Qt with the build tools (see build-qt.sh).
        extra+=("-DQT_HOST_PATH=$ZDEPS/$ZHOSTARCH")
    fi
    # ${extra[@]+...} instead of a bare "${extra[@]}": the system bash on the
    # mac is 3.2, where an empty array under set -u counts as an unset variable.
    "$prefix/bin/qt-cmake" -S "$ROOT" -B "$out" \
        -DCMAKE_BUILD_TYPE=Release \
        -DWITH_STATIC_QT=ON \
        -DCMAKE_OSX_DEPLOYMENT_TARGET="$ZMACOS_MIN" \
        -DCMAKE_OSX_ARCHITECTURES="$arch" \
        ${extra[@]+"${extra[@]}"}
    cmake --build "$out" -j8 --target zametti

    # The version comes from cmake, one for both halves.
    local version
    version="$(cat "$out/generated/version.txt")"
    [ -n "$version" ] || { echo "cmake did not write the version to $out/generated/version.txt" >&2; exit 1; }
    if [ -n "$APP_VERSION" ] && [ "$APP_VERSION" != "$version" ]; then
        echo "the halves disagree on the version: $APP_VERSION and $version" >&2
        exit 1
    fi
    APP_VERSION="$version"

    # ── Strip and acceptance of the slice ────────────────────────────────────
    local bin="$out/app/zametti"
    local before after
    before="$(stat -f %z "$bin")"
    strip "$bin"
    after="$(stat -f %z "$bin")"
    echo "  strip: $((before / 1048576)) MB → $((after / 1048576)) MB"

    echo "=== acceptance [$arch]: $bin ==="
    lipo -info "$bin"
    local minos
    minos="$(otool -l "$bin" | awk '/LC_BUILD_VERSION/{v=1} v && /minos/{print $2; exit}')"
    if [ "$minos" != "$ZMACOS_MIN" ]; then
        echo "slice $arch: floor $minos instead of $ZMACOS_MIN" >&2
        exit 1
    fi
    echo "  floor $minos — correct"
    # No foreign dynamic libraries may remain outside: only system frameworks.
    # The "path:" header sits in column 0, library lines start with a tab; we
    # keep only the latter.
    local foreign
    foreign="$(otool -L "$bin" | grep $'^\t' | grep -v -E '/usr/lib/|/System/Library/' || true)"
    if [ -n "$foreign" ]; then
        echo "foreign libraries remain outside:" >&2
        echo "$foreign" >&2
        exit 1
    fi
    echo "  no foreign libraries outside"
    # Smoke run: x86_64 runs under Rosetta. --help, --version and --dump-config
    # work without a display (--dump-config brings up an offscreen
    # QGuiApplication itself). The stripped binary must launch — that is what
    # we check.
    echo "  smoke run [$arch]:"
    arch "-$arch" "$bin" --help > /dev/null
    local said
    said="$(arch "-$arch" "$bin" --version)"
    if [ "$said" != "zametti $APP_VERSION" ]; then
        echo "--version said \"$said\", cmake said \"$APP_VERSION\"" >&2
        exit 1
    fi
    arch "-$arch" "$bin" --noconfig --dump-config > /dev/null
    echo "    --help, --version ($said) and --noconfig --dump-config passed"
}

# ── 2. Zametti.app and dmg of one half ───────────────────────────────────────
package_arch() {
    local arch="$1"
    local out="$UNI/$arch"
    local app="$out/$APP_NAME.app"
    rm -rf "$app"
    mkdir -p "$app/Contents/MacOS" "$app/Contents/Resources"
    cp "$out/app/zametti" "$app/Contents/MacOS/zametti"
    cp "$out/generated/Info.plist" "$app/Contents/Info.plist"
    cp "$ICNS" "$app/Contents/Resources/zametti.icns"

    codesign --force --sign - "$app"
    codesign --verify --strict --deep "$app"
    echo "=== $app signed (ad-hoc) and verified ==="

    local stage="$out/dmg-staging"
    rm -rf "$stage"
    mkdir -p "$stage"
    cp -R "$app" "$stage/"
    ln -s /Applications "$stage/Applications"
    local dmg="$UNI/$APP_NAME-$APP_VERSION-$(tag_of "$arch").dmg"
    rm -f "$dmg" "$out/raw.dmg"
    # NOT hdiutil create -srcfolder: it MOUNTS the image in /Volumes along the
    # way, and mounting may be forbidden (agent sandbox, CI). makehybrid builds
    # the file system directly, convert compresses to UDZO — neither mounts.
    hdiutil makehybrid -hfs -hfs-volume-name "$APP_NAME" -o "$out/raw.dmg" "$stage" > /dev/null
    hdiutil convert "$out/raw.dmg" -format UDZO -o "$dmg" > /dev/null
    rm -f "$out/raw.dmg"
    rm -rf "$stage"
    hdiutil verify "$dmg" > /dev/null
    echo "=== $dmg built and verified ==="
    RESULTS+=("$dmg")
}

RESULTS=()
make_icns
for arch in $ARCHS; do
    build_arch "$arch"
    package_arch "$arch"
done
rm -f "$ICNS"

echo
echo "=== results (version $APP_VERSION) ==="
for dmg in "${RESULTS[@]}"; do
    ls -lh "$dmg" | awk '{print " ", $9, "—", $5}'
done
for arch in $ARCHS; do
    echo "  $UNI/$arch/$APP_NAME.app — kept alongside, not deleted"
done
