#!/bin/bash
#
# Qt for Android end to end: static OpenSSL, then qtbase, then qtsvg —
# arm64-v8a, Android 15+, into $ZANDROID_PREFIX. Shared libraries: Qt for
# Android cannot be static, the Java side loads the .so files from the APK.
#
# One-time toolchain, all of it under $ZDEPS (owner's word, 03.10.2026: no
# brew, no sudo, nothing outside ~/ai/work):
#
#   source packaging/android/zenv.sh
#   mkdir -p "$ZBUILD" && cd "$ZBUILD"
#   curl -fsSLO https://dl.google.com/android/repository/commandlinetools-mac_arm64-15859902_latest.zip
#   curl -fsSL -o temurin-21-mac-aarch64.tar.gz \
#       "https://api.adoptium.net/v3/binary/latest/21/ga/mac/aarch64/jdk/hotspot/normal/eclipse?project=jdk"
#   curl -fsSLO https://github.com/openssl/openssl/releases/download/openssl-$ZOPENSSL_VERSION/openssl-$ZOPENSSL_VERSION.tar.gz
#   mkdir jdk-21.tmp && tar -C jdk-21.tmp -xzf temurin-21-mac-aarch64.tar.gz \
#       && mv jdk-21.tmp/jdk-21* "$ZDEPS/jdk-21" && rmdir jdk-21.tmp
#   mkdir -p "$ANDROID_SDK_ROOT/cmdline-tools" \
#       && unzip -q commandlinetools-mac_arm64-15859902_latest.zip -d "$ANDROID_SDK_ROOT/cmdline-tools" \
#       && mv "$ANDROID_SDK_ROOT/cmdline-tools/cmdline-tools" "$ANDROID_SDK_ROOT/cmdline-tools/latest"
#   yes | sdkmanager --sdk_root="$ANDROID_SDK_ROOT" --licenses
#   sdkmanager --sdk_root="$ANDROID_SDK_ROOT" "platform-tools" \
#       "platforms;android-$ZANDROID_TARGET_SDK" "build-tools;$ZANDROID_BUILD_TOOLS" "ndk;$ZANDROID_NDK_VERSION"
#
# Qt sources are the same v6.10.3 trees the mac build uses ($ZBUILD/qtbase,
# $ZBUILD/qtsvg, clone commands in packaging/mac/build-qt.sh); the Android
# build goes out-of-source into its own directories, so the two never meet.
# Same version as mac and Linux (the owner's word): one Qt, one set of layout
# bugs, one set of acceptance snapshots.
#
# The mac patches (packaging/mac/patches) are already applied to the shared
# source tree by the mac script and are NOT touched here: qtbase-arm-yield-acle
# is about AppleClang's <arm_acle.h> and is guarded by __has_include, so under
# the NDK's clang it is harmless either way.
#
# 16 KB PAGES. Qt 6.10 links everything with -Wl,-z,max-page-size=16384 by
# itself (feature android_16kb_pages, qtbase/cmake/QtPlatformTargetHelpers.cmake)
# and exports the flag through Qt6::Platform to the program too. The check at
# the end is a guard, not the mechanism.
#
#   bash packaging/android/build-qt.sh
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=zenv.sh
source "$HERE/zenv.sh"

for d in "$ZBUILD/qtbase" "$ZBUILD/qtsvg" "$ANDROID_NDK_ROOT" \
         "$ANDROID_SDK_ROOT/platforms/android-$ZANDROID_TARGET_SDK" \
         "$ANDROID_SDK_ROOT/build-tools/$ZANDROID_BUILD_TOOLS" "$JAVA_HOME"; do
    [ -d "$d" ] || { echo "missing $d (the toolchain commands are in the script header)" >&2; exit 1; }
done
[ -x "$ZHOSTQT/bin/androiddeployqt" ] || {
    echo "host Qt without androiddeployqt: $ZHOSTQT — first: ZARCHS=arm64 bash packaging/mac/build-qt.sh" >&2
    exit 1
}
[ -x "$ZNDK_BIN/clang" ] || { echo "no clang in $ZNDK_BIN" >&2; exit 1; }

# --- OpenSSL, static ---------------------------------------------------------
#
# TLS for QtNetwork: the mac has SecureTransport and Linux links OpenSSL into
# the static Qt; Android has neither a system OpenSSL for apps nor anything
# Qt could use instead. Linked INTO libQt6Network.so (-openssl-linked below)
# rather than carried as libssl_3.so/libcrypto_3.so next to it: one copy, no
# dlopen at run time, no QT_ANDROID_EXTRA_LIBS. The core itself does not use
# OpenSSL (the sync's crypto is libsodium, vendored), so this is the only
# copy in the APK. Version and options as in docs/zametti-build-linux.md.
#
# -fPIC: the static archives end up inside a shared library. The NDK's clang
# defaults to PIC for android targets anyway; stated for parity with Linux.
ossl_src="$ZBUILD/openssl-$ZOPENSSL_VERSION"
ossl_tar="$ZBUILD/openssl-$ZOPENSSL_VERSION.tar.gz"
if [ ! -f "$ZANDROID_SYSROOT/lib/libssl.a" ] || [ ! -f "$ZANDROID_SYSROOT/lib/libcrypto.a" ]; then
    [ -d "$ossl_src" ] || { [ -f "$ossl_tar" ] || { echo "no $ossl_tar (see the header)" >&2; exit 1; }
                            tar -C "$ZBUILD" -xzf "$ossl_tar"; }
    echo "=== OpenSSL $ZOPENSSL_VERSION [android-arm64] → $ZANDROID_SYSROOT ==="
    cd "$ossl_src"
    [ -f Makefile ] && make distclean >/dev/null 2>&1 || true
    PATH="$ZNDK_BIN:$PATH" ./Configure android-arm64 \
        -D__ANDROID_API__="$ZANDROID_MIN_SDK" \
        no-shared no-tests no-docs no-apps no-legacy \
        --prefix="$ZANDROID_SYSROOT" --libdir=lib --openssldir=/system/etc/security \
        -fPIC -O2
    PATH="$ZNDK_BIN:$PATH" make -j8 build_libs
    PATH="$ZNDK_BIN:$PATH" make install_dev
    [ -f "$ZANDROID_SYSROOT/lib/libssl.a" ] && [ -f "$ZANDROID_SYSROOT/lib/libcrypto.a" ]
else
    echo "=== OpenSSL already in $ZANDROID_SYSROOT ==="
fi

# --- qtbase ------------------------------------------------------------------
#
# How to read the flag list — as in the mac script: -qt- our own copy, -no-
# not needed. Under the NDK the host's libraries are invisible to configure
# anyway (find-root mode), so the bundled copies would be picked silently;
# naming them makes the configure deterministic rather than lucky. zlib is the
# one the NDK sysroot does provide; we still take Qt's own.
#
# -android-ndk-platform is the minimum Android of the Qt libraries (API 35);
# the program repeats it as QT_ANDROID_MIN_SDK_VERSION in android/CMakeLists.txt.
# -android-abis takes exactly ONE abi in Qt 6.
bdir="$ZBUILD/qtbase-build-android-$ZANDROID_ABI"
echo "=== qtbase [android $ZANDROID_ABI] → $ZANDROID_PREFIX ==="
mkdir -p "$bdir"
cd "$bdir"
"$ZBUILD/qtbase/configure" \
    -prefix "$ZANDROID_PREFIX" \
    -platform android-clang \
    -android-sdk "$ANDROID_SDK_ROOT" -android-ndk "$ANDROID_NDK_ROOT" \
    -android-abis "$ZANDROID_ABI" -android-ndk-platform "android-$ZANDROID_MIN_SDK" \
    -qt-host-path "$ZHOSTQT" \
    -release \
    -opensource -confirm-license \
    -nomake examples -nomake tests \
    -qt-pcre -qt-harfbuzz -qt-doubleconversion -qt-libjpeg -qt-libpng -qt-freetype -qt-zlib \
    -no-glib -no-icu -no-cups -no-dbus \
    -openssl-linked \
    -- \
    -DOPENSSL_ROOT_DIR="$ZANDROID_SYSROOT" \
    -DOPENSSL_USE_STATIC_LIBS=ON
cmake --build . -j8
cmake --install .

# --- qtsvg -------------------------------------------------------------------
#
# The toolbar icons are SVG and the program links Qt6::Svg as a module (the
# argument is in app/CMakeLists.txt). qt-cmake from the prefix supplies the
# toolchain, the ABI, the NDK and the SDK; QT_HOST_PATH is demanded again by
# the toolchain file when cross-building — we pass the same one.
sdir="$ZBUILD/qtsvg-build-android-$ZANDROID_ABI"
echo "=== qtsvg [android $ZANDROID_ABI] ==="
mkdir -p "$sdir"
cd "$sdir"
"$ZANDROID_PREFIX/bin/qt-cmake" "$ZBUILD/qtsvg" -DCMAKE_BUILD_TYPE=Release \
    -DQT_HOST_PATH="$ZHOSTQT"
cmake --build . -j8
cmake --install .

# --- acceptance --------------------------------------------------------------
echo "=== 16 KB check over $ZANDROID_PREFIX ==="
check16k "$ZANDROID_PREFIX"/lib/libQt6*.so "$ZANDROID_PREFIX"/plugins/*/*.so

# OpenSSL must be INSIDE the TLS plugin. In Qt 6 the TLS backend is a plugin
# (plugins/tls/libplugins_tls_qopensslbackend), and -openssl-linked links the
# static archives into THAT library, not into libQt6Network: SSL_new defined
# there, none imported. Otherwise the APK would need loose libssl/libcrypto
# and TLS would fail at the first connection.
tls="$ZANDROID_PREFIX/plugins/tls/libplugins_tls_qopensslbackend_$ZANDROID_ABI.so"
[ -f "$tls" ] || { echo "no OpenSSL TLS plugin: $tls" >&2; exit 1; }
if "$ZNDK_BIN/llvm-nm" -D "$tls" | grep -q ' U SSL_new'; then
    echo "OpenSSL is NOT linked into the TLS plugin (SSL_new imported)" >&2; exit 1
fi
"$ZNDK_BIN/llvm-nm" -D --defined-only "$tls" | grep -q ' SSL_new' || {
    echo "OpenSSL is NOT linked into the TLS plugin (no SSL_new at all)" >&2; exit 1
}
echo "TLS plugin: $(strings "$tls" | grep -m1 -E 'OpenSSL 3\.' || echo 'OpenSSL version string not found')"
echo "=== Qt for Android ready: $ZANDROID_PREFIX ==="
