#!/bin/bash
#
# The program for Android, from cmake to an installable APK:
#
#   bash packaging/android/build-app.sh            # build-android/… zametti-android.apk
#   bash packaging/android/build-app.sh install    # + adb install -r
#
# NATIVE CODE IS RELEASE, THE APK IS A DEBUG BUILD — and that is on purpose.
# CMAKE_BUILD_TYPE=Release compiles our code and Qt's with optimisation; the
# APK, though, is assembled by gradle, and a Release assembly comes out
# UNSIGNED (zametti-android-release-unsigned.apk — Qt passes --release to
# androiddeployqt on a Release configuration and signs only with a keystore
# handed over through QT_ANDROID_SIGN_APK + QT_ANDROID_KEYSTORE_*). An
# unsigned APK does not install. QT_ANDROID_DEPLOYMENT_TYPE=DEBUG keeps the
# optimised libraries and makes gradle run assembleDebug: signed with the
# automatic debug keystore, android:debuggable=true — which `run-as`, the only
# way to seed the private store from the mac (device.sh seed), requires anyway.
# Signing for the store is a later brief.
#
# qt-cmake from the Android prefix exports its own toolchain file, which
# chain-loads the NDK toolchain Qt was configured with and remembers the SDK,
# ABI, platform and STL. Host path and SDK root are repeated explicitly so that
# a moved SDK fails here, loudly, and not deep inside gradle.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
# shellcheck source=zenv.sh
source "$HERE/zenv.sh"

INSTALL=0
case "${1:-}" in
    "") ;;
    install) INSTALL=1 ;;
    *) echo "usage: bash packaging/android/build-app.sh [install]" >&2; exit 2 ;;
esac

[ -x "$ZANDROID_PREFIX/bin/qt-cmake" ] || {
    echo "no Qt for Android in $ZANDROID_PREFIX — first: bash packaging/android/build-qt.sh" >&2
    exit 1
}
[ -d "$JAVA_HOME" ] || { echo "no JDK at $JAVA_HOME" >&2; exit 1; }

OUT="$ROOT/build-android"
APK="$OUT/android/android-build/zametti-android.apk"

echo "=== configure → $OUT ==="
"$ZANDROID_PREFIX/bin/qt-cmake" -S "$ROOT" -B "$OUT" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DQT_HOST_PATH="$ZHOSTQT" \
    -DANDROID_SDK_ROOT="$ANDROID_SDK_ROOT" \
    -DQT_ANDROID_DEPLOYMENT_TYPE=DEBUG \
    -DQT_ANDROID_SDK_BUILD_TOOLS_REVISION="$ZANDROID_BUILD_TOOLS"

# Two explicit targets, not a bare `cmake --build`: the global apk target is
# part of ALL for user projects, and a bare build would run gradle for every
# executable it finds. One build at a time; gradle's JVM is capped in zenv.sh.
echo "=== build libzametti-android ==="
cmake --build "$OUT" -j8 --target zametti-android
echo "=== apk ==="
cmake --build "$OUT" -j8 --target zametti-android_make_apk
[ -f "$APK" ] || { echo "no APK at $APK" >&2; exit 1; }

# --- acceptance --------------------------------------------------------------
#
# Everything is judged on the APK's contents, not on the prefix: that is what
# the phone loads.
tmp="$OUT/apk-check"
rm -rf "$tmp"
mkdir -p "$tmp"
unzip -q -o "$APK" 'lib/*' -d "$tmp"

echo "=== 16 KB check over the APK ==="
check16k "$tmp"/lib/*/*.so

# arm64-v8a and nothing else
abis="$(ls "$tmp/lib")"
[ "$abis" = "$ZANDROID_ABI" ] || { echo "unexpected ABIs in the APK: $abis" >&2; exit 1; }

# OpenSSL lives inside libQt6Network; a loose copy would mean the Qt build
# went the dlopen way and TLS would depend on what else is in the APK.
if ls "$tmp/lib/$ZANDROID_ABI" | grep -q -E '^lib(ssl|crypto)'; then
    echo "loose OpenSSL libraries in the APK" >&2; exit 1
fi

# NEEDED of our own library: the system's, the NDK's libc++, Qt's — nothing
# else, because nothing else gets packaged (androiddeployqt copies Qt and
# libc++_shared.so only). A stray libomp.so or libz.so would load fine on the
# mac and fail on the phone.
our="$tmp/lib/$ZANDROID_ABI/libzametti-android_$ZANDROID_ABI.so"
[ -f "$our" ] || { echo "no $our in the APK" >&2; exit 1; }
stray="$("$ZNDK_BIN/llvm-readelf" -d "$our" | awk '/NEEDED/ { gsub(/[][]/, "", $NF); print $NF }' \
    | grep -v -E '^(libc|libm|libdl|liblog|libandroid|libz|libGLESv2|libEGL|libc\+\+_shared|libQt6.*)\.so$' || true)"
[ -z "$stray" ] || { echo "unexpected NEEDED in $our:" >&2; echo "$stray" >&2; exit 1; }

echo "=== $(cat "$OUT/generated/version.txt") → $APK ==="
ls -l "$APK" | awk '{ printf "  %.1f MB  %s\n", $5/1048576, $9 }'
echo "  libraries in the APK:"
ls -l "$tmp/lib/$ZANDROID_ABI" | awk 'NR > 1 { printf "  %8.1f MB  %s\n", $5/1048576, $9 }'
rm -rf "$tmp"

if [ "$INSTALL" = 1 ]; then
    echo "=== adb install ==="
    adb install -r "$APK"
fi
