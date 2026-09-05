# Shared environment for the Windows cross-build of zametti. Use it like this:
#
#   source packaging/win/zenv.sh
#
# Same idea as packaging/linux/zenv.sh: Qt, the third-party libraries and the
# program itself are all built with ONE compiler and ONE set of flags. Here that
# is the host's mingw-w64 -- the target world (/usr/x86_64-w64-mingw32) does not
# overlap the host one, so no sysroot is needed, and the API floor is set by
# _WIN32_WINNT in the toolchain.
#
# Paths are given from outside (`ZWPREFIX=/other/place source packaging/win/zenv.sh`).

export ZWTRIPLE="${ZWTRIPLE:-x86_64-w64-mingw32}"
export ZWPREFIX="${ZWPREFIX:-$HOME/work/zwin}"    # where the built output (Qt) goes
export ZWBUILD="${ZWBUILD:-$HOME/work/zbuild}"    # where we build (shared with linux)

# Host Qt of THE SAME version -- the cross-build takes moc, rcc, uic and syncqt
# from it. We use the static build of the portable Linux recipe: the version
# there is 6.10.3, and the tools in libexec/ are ordinary host programs, so
# being static does not get in their way.
export QT_HOST_PATH="${QT_HOST_PATH:-$HOME/work/zdeps}"

export ZWTOOLCHAIN="${ZWTOOLCHAIN:-$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/toolchains/mingw-w64.cmake}"

export CC="${ZWTRIPLE}-gcc"
export CXX="${ZWTRIPLE}-g++"

# pkg-config for the target: mingw puts its .pc files next to the libraries.
# LIBDIR (not PATH) -- so that the host .pc files are not visible at all.
export PKG_CONFIG_LIBDIR="/usr/${ZWTRIPLE}/lib/pkgconfig:$ZWPREFIX/lib/pkgconfig"
unset PKG_CONFIG_SYSROOT_DIR

if ! command -v "$CC" >/dev/null 2>&1; then
    echo "zenv-win: no $CC -- install mingw-w64:" >&2
    echo "          sudo apt install mingw-w64" >&2
fi
if [ ! -x "$QT_HOST_PATH/libexec/moc" ]; then
    echo "zenv-win: QT_HOST_PATH=$QT_HOST_PATH has no libexec/moc --" >&2
    echo "          the Qt cross-build will have nothing to process headers with" >&2
fi
echo "zenv-win: triple=$ZWTRIPLE prefix=$ZWPREFIX host-qt=$QT_HOST_PATH"
