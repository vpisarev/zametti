#!/bin/bash
#
# The phone side of the Android brief (step 0): seeding the private store,
# starting the program, reading the numbers. Everything goes through adb;
# the package is a debug build (see build-app.sh), so `run-as` works.
#
#   bash packaging/android/device.sh seed <store-dir>   # tar | adb exec-in run-as … into files/vpnotes
#   bash packaging/android/device.sh start              # am start -W: cold start, TotalTime
#   bash packaging/android/device.sh stop               # force-stop (for a cold start)
#   bash packaging/android/device.sh log                # logcat, [perf] lines only
#   bash packaging/android/device.sh mem                # dumpsys meminfo: PSS total
#   bash packaging/android/device.sh apk                # size and the libraries inside
#   bash packaging/android/device.sh shot <file.png>    # screenshot
#
# The store on the phone is called vpnotes whatever the directory on the mac
# is called: the program opens exactly that name (android/main.cpp). The
# source is a COPY of the owner's store and is only read here.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
# shellcheck source=zenv.sh
source "$HERE/zenv.sh" >/dev/null

PKG=io.zametti.zametti
ACTIVITY="$PKG/org.qtproject.qt.android.bindings.QtActivity"
APK="$ROOT/build-android/android/android-build/zametti-android.apk"

cmd="${1:-}"
case "$cmd" in
    seed)
        src="${2:-}"
        [ -d "$src/.zametti" ] || { echo "not a store: $src" >&2; exit 2; }
        parent="$(cd "$(dirname "$src")" && pwd)"
        name="$(basename "$src")"
        echo "=== $src → $PKG/files/vpnotes ==="
        adb shell run-as "$PKG" rm -rf files/vpnotes
        # tar on the mac streams the directory under its own name; the phone
        # unpacks and the name is fixed afterwards. Binary-safe, one pipe.
        tar -C "$parent" -cf - "$name" | adb exec-in run-as "$PKG" tar -xf - -C files
        [ "$name" = vpnotes ] || adb shell run-as "$PKG" mv "files/$name" files/vpnotes
        adb shell run-as "$PKG" sh -c 'ls files/vpnotes | wc -l; du -sk files/vpnotes'
        ;;
    start)
        adb shell am start -W -n "$ACTIVITY" | grep -E 'TotalTime|WaitTime|Status'
        ;;
    stop)
        adb shell am force-stop "$PKG"
        ;;
    log)
        adb logcat -v time -s zametti
        ;;
    mem)
        adb shell dumpsys meminfo "$PKG" | grep -E 'TOTAL PSS|TOTAL RSS|Native Heap|Graphics' | head -6
        ;;
    apk)
        [ -f "$APK" ] || { echo "no $APK" >&2; exit 1; }
        ls -l "$APK" | awk '{ printf "%.1f MB  %s\n", $5/1048576, $9 }'
        unzip -l "$APK" | awk '/lib\/.*\.so$/ { printf "%8.1f MB  %s\n", $1/1048576, $4 }' | sort -rn
        ;;
    shot)
        out="${2:-}"
        [ -n "$out" ] || { echo "usage: device.sh shot <file.png>" >&2; exit 2; }
        adb exec-out screencap -p > "$out"
        ls -l "$out"
        ;;
    *)
        sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//' >&2
        exit 2
        ;;
esac
