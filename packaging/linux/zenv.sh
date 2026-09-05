# Common environment for portable zametti builds on Linux. Use as:
#
#   source packaging/linux/zenv.sh
#
# The point: all third-party libraries and the program itself are built with
# THE SAME switches against the Ubuntu 20.04 sysroot. Diverging flags between
# Qt and the program are the classic way to get a binary that builds but does
# not run.
#
# Paths may be set from outside (`ZSYS=/other/place source packaging/linux/zenv.sh`);
# otherwise the usual ones are used. The file used to live inside the sysroot
# itself (`~/work/zsys/bin/zenv.sh`) and moved along with it; now it is in the
# repository so that the recipe in docs/zametti-build-linux.md reproduces on a
# new machine fully, not halfway.

export ZSYS="${ZSYS:-$HOME/work/zsys}"        # sysroot (replaced by unpacking the tarball)
export ZPREFIX="${ZPREFIX:-$HOME/work/zdeps}" # where built results go (NOT overwritten by the tarball)
export ZBUILD="${ZBUILD:-$HOME/work/zbuild}"  # where we build

# The wrappers, not gcc-15 itself: they append --sysroot and LD_LIBRARY_PATH
# for the focal hostlibs. Their source is packaging/linux/sysroot-bin/, and
# packaging/linux/fix-sysroot.sh puts them into $ZSYS/bin.
export CC="$ZSYS/bin/gcc"
export CXX="$ZSYS/bin/g++"

# pkg-config looks ONLY in the sysroot: LIBDIR (not PATH) cuts off the host's
# system directories, otherwise .pc files from a recent Ubuntu would be found.
# usr/share/pkgconfig is mandatory: that is where wayland-protocols.pc lives.
export PKG_CONFIG_SYSROOT_DIR="$ZSYS"
export PKG_CONFIG_LIBDIR="$ZSYS/usr/lib/x86_64-linux-gnu/pkgconfig:$ZSYS/usr/share/pkgconfig"

# -fPIC is mandatory: the static libraries end up inside a PIE program.
export ZCFLAGS="-O2 -fPIC"
# Mandatory, not optional: libstdc++ of gcc-15 requires GLIBCXX_3.4.32, which
# is absent even in the Ubuntu 20.04 the sysroot was taken from.
export ZLDFLAGS="-static-libstdc++ -static-libgcc"

if [ ! -x "$CC" ]; then
    echo "zenv: $CC not found — prepare the sysroot first:" >&2
    echo "        bash packaging/linux/fix-sysroot.sh" >&2
fi
echo "zenv: sysroot=$ZSYS prefix=$ZPREFIX cc=$(basename "$CC")"
