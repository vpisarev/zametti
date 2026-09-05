#!/bin/bash
#
# Prepare the sysroot after unpacking zsys.tar. RUN EVERY TIME after
# `tar -xf zsys.tar`: the archive brings the absolute symlinks back.
#
#   bash packaging/linux/fix-sysroot.sh            # sysroot taken from $ZSYS
#   bash packaging/linux/fix-sysroot.sh /other/path
#
# Does two things.
#
# 1. FIXES SYMLINKS. In Ubuntu the dev symlinks like libpthread.so point by
#    ABSOLUTE path to /lib/x86_64-linux-gnu/libpthread.so.0. Inside a sysroot
#    such a path is resolved not by the linker but by the kernel, and from the
#    REAL root, i.e. the host. As a result -lpthread, -ldl, -lrt, -luuid and
#    friends would silently come from the host glibc instead of the sysroot,
#    and portability would leak without a single message. Where the host lacks
#    the needed version (libdbus-1), the symlink is simply dangling and the
#    linker fails; that is luck, not the norm.
#
#    Fixed by rewriting to a RELATIVE path: it resolves inside the sysroot and
#    no longer depends on the host. Only links whose target exists in the
#    sysroot are touched; the rest (aspell in /var/lib, /etc/alternatives) are
#    left as is: they are junk anyway and irrelevant to the build.
#
# 2. INSTALLS THE COMPILER WRAPPERS into $ZSYS/bin. Their source is sysroot-bin/
#    next to this file, i.e. in the repository; they get into the sysroot as a
#    copy. Previously they lived only in the sysroot, and the build recipe on a
#    new machine reproduced only halfway. The wrappers compute the root from
#    their own location, so they work exactly where they are installed and
#    nowhere else: call them by path, $ZSYS/bin/gcc, not from here.
set -e

ROOT="${1:-$ZSYS}"
if [ -z "$ROOT" ]; then
    echo "sysroot not set: neither as an argument nor in \$ZSYS" >&2
    echo "  source packaging/linux/zenv.sh" >&2
    exit 1
fi
ROOT="$(cd "$ROOT" && pwd)"
[ -d "$ROOT/usr/include" ] || { echo "$ROOT does not look like a sysroot: no usr/include" >&2; exit 1; }

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

cd "$ROOT"
fixed=0; skipped=0
while IFS= read -r link; do
    target=$(readlink "$link")
    inside="$ROOT/${target#/}"
    if [ ! -e "$inside" ]; then
        skipped=$((skipped + 1))
        continue
    fi
    rel=$(realpath -m --relative-to="$(dirname "$link")" "${target#/}")
    ln -sfn "$rel" "$link"
    fixed=$((fixed + 1))
done < <(find usr lib lib64 -type l -lname '/*' 2>/dev/null)

mkdir -p "$ROOT/bin"
install -m 755 "$HERE/sysroot-bin/gcc" "$HERE/sysroot-bin/g++" "$ROOT/bin/"

echo "sysroot: $ROOT"
echo "  symlinks rewritten to relative: $fixed"
echo "  left as is (target not in sysroot): $skipped"
echo "  compiler wrappers: $ROOT/bin/{gcc,g++}"

# hostlibs: the four focal libraries (libisl, libbfd, libopcodes) without which
# the focal gcc-15 does not start on the host. Binary files, they do not belong
# in the repository; but their absence must not go unmentioned: without them
# the wrapper fails on the very first call, and the linker message will not
# name the cause.
if [ ! -d "$ROOT/hostlibs" ]; then
    echo "  WARNING: no $ROOT/hostlibs — gcc-15 from the sysroot will not start on the host." >&2
    echo "           It should hold libisl/libbfd/libopcodes from the same Ubuntu 20.04." >&2
fi
"$ROOT/bin/gcc" --version >/dev/null || { echo "  wrapper does not start" >&2; exit 1; }
