# Shared environment for the Android build of zametti. Use it like this:
#
#   source packaging/android/zenv.sh
#
# Same scheme as packaging/mac: zenv.sh (this file), build-qt.sh (OpenSSL +
# qtbase + qtsvg for Android, shared libraries — a static Qt for Android does
# not exist, the Java side loads the .so files from the APK), build-app.sh (the
# program from cmake to the APK) and, only here, device.sh: seeding the store,
# starting the app and reading the numbers off the phone — the mac has no
# "device" step, Android has nothing else.
#
# ONE ABI, ONE DEVICE CLASS. arm64-v8a only, Android 15+ (API 35): the brief
# names a real phone, not the emulator, and 16 KB page alignment of every .so
# is mandatory from Android 15 on. The emulator (x86_64) would need a second
# Qt prefix — out of scope.
#
# THE TOOLCHAIN LIVES IN $ZDEPS, NOT IN /opt OR /Library (the owner's word,
# 03.10.2026): JDK and SDK are unpacked from tarballs into $ZDEPS so that the
# whole thing is reproducible from zenv.sh and nothing outside ~/ai/work is
# touched. The download commands are in the header of build-qt.sh.
#
# Paths can be overridden from outside: ZDEPS=/other source packaging/android/zenv.sh

export ZDEPS="${ZDEPS:-$HOME/ai/work/zdeps}"
export ZBUILD="${ZBUILD:-$ZDEPS/build}"      # sources, tarballs and build trees (safe to wipe)

# HOST QT. Qt for Android is a cross build and needs the host's moc/rcc/uic
# and androiddeployqt of THE SAME VERSION. The static macOS Qt 6.10.3 built by
# packaging/mac/build-qt.sh has them all (libexec/, bin/androiddeployqt,
# lib/cmake/Qt6HostInfo) — a static host build qualifies, Qt only runs its
# tools from there.
export ZHOSTQT="${ZHOSTQT:-$ZDEPS/arm64}"

# TARGET QT and the OpenSSL it links. OpenSSL sits in its own tree so that
# OPENSSL_ROOT_DIR points at nothing but OpenSSL.
export ZANDROID_PREFIX="${ZANDROID_PREFIX:-$ZDEPS/android-arm64}"
export ZANDROID_SYSROOT="${ZANDROID_SYSROOT:-$ZDEPS/android-arm64-sysroot}"

export ZANDROID_ABI=arm64-v8a
export ZANDROID_MIN_SDK="${ZANDROID_MIN_SDK:-35}"        # Android 15
export ZANDROID_TARGET_SDK="${ZANDROID_TARGET_SDK:-36}"  # Android 16
export ZANDROID_BUILD_TOOLS="${ZANDROID_BUILD_TOOLS:-36.0.0}"
# Qt 6.10 is built and tested with NDK r27c, and nothing else; r28 would
# change the 16 KB defaults and the libc++ it ships.
export ZANDROID_NDK_VERSION="${ZANDROID_NDK_VERSION:-27.2.12479018}"
export ZOPENSSL_VERSION="${ZOPENSSL_VERSION:-3.5.7}"

# SDK and NDK. ANDROID_HOME is what gradle reads, ANDROID_SDK_ROOT is what
# Qt's configure and androiddeployqt read; they are the same directory.
export ANDROID_SDK_ROOT="${ANDROID_SDK_ROOT:-$ZDEPS/android-sdk}"
export ANDROID_HOME="$ANDROID_SDK_ROOT"
export ANDROID_NDK_ROOT="${ANDROID_NDK_ROOT:-$ANDROID_SDK_ROOT/ndk/$ZANDROID_NDK_VERSION}"
# OpenSSL's Configure looks for the NDK under this older name.
export ANDROID_NDK_HOME="$ANDROID_NDK_ROOT"
# The NDK's host tag on macOS is darwin-x86_64 even on Apple Silicon: the
# binaries are universal.
export ZNDK_BIN="$ANDROID_NDK_ROOT/toolchains/llvm/prebuilt/darwin-x86_64/bin"

# JDK 21 (Temurin): gradle 8.14 and AGP 8.10 need 17+, androiddeployqt takes
# jarsigner/keytool from JAVA_HOME.
export JAVA_HOME="${JAVA_HOME:-$ZDEPS/jdk-21/Contents/Home}"

# Gradle spawns a JVM daemon of its own; cap it so that it does not sit next
# to a -j8 build with two gigabytes of heap (CLAUDE.md memory rule).
export GRADLE_OPTS="${GRADLE_OPTS:--Dorg.gradle.jvmargs=-Xmx2g}"

export PATH="$JAVA_HOME/bin:$ANDROID_SDK_ROOT/platform-tools:$ANDROID_SDK_ROOT/cmdline-tools/latest/bin:$PATH"

# check16k <lib.so>...: every PT_LOAD segment of every named library must be
# aligned to a multiple of 16 KiB, otherwise Android 15+ refuses to load it.
# Qt 6.10 passes -Wl,-z,max-page-size=16384 itself (feature android_16kb_pages,
# exported through Qt6::Platform to our own library too), so this is the GUARD,
# not the mechanism — it also covers libc++_shared.so, which comes from the
# NDK and which Qt's flag does not control. Non-zero exit lists the offenders.
# Written for the mac's bash 3.2: no mapfile, no associative arrays.
check16k() {
    local readelf="$ZNDK_BIN/llvm-readelf" bad=0 f line align
    [ -x "$readelf" ] || { echo "check16k: no $readelf" >&2; return 2; }
    for f in "$@"; do
        while read -r line; do
            [ -n "$line" ] || continue
            align="${line##* }"
            if [ $(( align % 16384 )) -ne 0 ]; then
                echo "16k: $f: LOAD segment aligned to $align" >&2
                bad=1
            fi
        done <<EOT
$("$readelf" -l "$f" | awk '$1 == "LOAD" { print $NF }')
EOT
    done
    return $bad
}

echo "zenv(android): sdk=$ANDROID_SDK_ROOT ndk=$ZANDROID_NDK_VERSION jdk=$JAVA_HOME qt=$ZANDROID_PREFIX host-qt=$ZHOSTQT min=$ZANDROID_MIN_SDK target=$ZANDROID_TARGET_SDK"
