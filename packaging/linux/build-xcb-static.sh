#!/bin/bash
#
# Build the xcb helper libraries as static archives with -fPIC into $ZPREFIX.
#
#   packaging/linux/build-xcb-static.sh
#
# WHY. The six libraries pulled in by Qt's xcb plugin live in packages with
# priority=EXTRA, the lowest priority there is: they are never part of a base
# install. One of them, libxcb-cursor0, is the single most common complaint
# about Qt 6.5+ ("could not load the Qt platform plugin xcb"). Linking them in
# removes six packages from the install requirements.
#
# WHY FROM UPSTREAM AND NOT THE ARCHIVES IN THE SYSROOT. Two reasons, both
# measured.
#
# First: Ubuntu builds libxcb-image.a and libxcb-util.a WITHOUT -fPIC, so they
# cannot go into a PIE program at all ("relocation R_X86_64_32S ... can not be
# used when making a PIE object").
#
# Second, and more important. libxcb-cursor has a built-in list of directories
# to search for cursor themes, and that list CHANGED:
#
#   focal:  ~/.icons:/usr/share/icons:/usr/share/pixmaps:/usr/X11R6/lib/X11/icons
#   now:    ~/.local/share/icons:~/.icons:/usr/share/icons:/usr/share/pixmaps
#
# ~/.local/share/icons appeared, which is exactly where GNOME and KDE put user
# themes today. Linking in the focal copy would give a program that silently
# fails to see the user's cursor theme. The other five are pure computational
# helpers: no baked-in paths, public API unchanged, so their version does not
# matter and they are taken from upstream along with it.

# BUILD MACHINE REQUIREMENTS (not target): m4. Two of the six packages,
# xcb-util-wm and xcb-util-cursor, generate part of their sources with it, and
# without it their configure fails at step five with a message that does not
# show the cause. So we check here and say it plainly.

set -e
# WE SET UP THE ENVIRONMENT OURSELVES: no need to `source zenv.sh` before this
# script (values set outside are left alone: ZSYS=/other/place works as before).
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$HERE/zenv.sh"
[ -n "$ZBUILD" ] || { echo "ZBUILD is not set" >&2; exit 1; }
command -v m4 >/dev/null || {
    echo "m4 not found — xcb-util-wm and xcb-util-cursor generate sources with it." >&2
    echo "  sudo apt install m4" >&2
    exit 1
}

mkdir -p "$ZBUILD/xcb" && cd "$ZBUILD/xcb"

# Order matters: each package may depend on the previous ones.
packages="xcb-util-0.4.1 xcb-util-image-0.4.1 xcb-util-keysyms-0.4.1
          xcb-util-renderutil-0.3.10 xcb-util-wm-0.4.2 xcb-util-cursor-0.1.6"

for p in $packages; do
    echo "=== $p ==="
    if [ ! -d "$p" ]; then
        curl -fsSLO "https://xcb.freedesktop.org/dist/$p.tar.xz" 2>/dev/null \
            || curl -fsSLO "https://xcb.freedesktop.org/dist/$p.tar.gz"
        tar -xf "$p".tar.*
    fi
    cd "$p"

    # Our own, already built, libraries are found via explicit flags, not via
    # PKG_CONFIG_PATH: PKG_CONFIG_SYSROOT_DIR from zenv.sh would prepend the
    # sysroot to the $ZPREFIX paths, yielding nonsense like $ZSYS$ZPREFIX/include.
    own_cflags="-I$ZPREFIX/include"

    # --with-cursorpath IS MANDATORY, and here is why. By default xcb-util-cursor
    # bakes into the code a cursor theme search path BUILT FROM ${datadir}, i.e.
    # from our --prefix. Without this switch the library ended up with
    #
    #   ~/.local/share/icons:~/.icons:$ZPREFIX/share/icons:$ZPREFIX/share/pixmaps
    #
    # so the program would look for system cursors in the build directory, which
    # does not exist on another machine at all. Noticed only by running `strings`
    # on the finished archive after the build; the build itself passes silently
    # and successfully. We set the target system's path, not our own.
    cursorpath='~/.local/share/icons:~/.icons:/usr/share/icons:/usr/share/pixmaps'

    ./configure --prefix="$ZPREFIX" --enable-static --disable-shared --with-pic \
        --with-cursorpath="$cursorpath" \
        CC="$CC" CFLAGS="-O2 -fPIC" \
        XCB_UTIL_CFLAGS="$own_cflags" XCB_UTIL_LIBS="-L$ZPREFIX/lib -lxcb-util" \
        XCB_IMAGE_CFLAGS="$own_cflags" XCB_IMAGE_LIBS="-L$ZPREFIX/lib -lxcb-image" \
        XCB_RENDERUTIL_CFLAGS="$own_cflags" XCB_RENDERUTIL_LIBS="-L$ZPREFIX/lib -lxcb-render-util" \
        > configure.log 2>&1 || { echo "  configure FAILED"; tail -20 configure.log; exit 1; }
    make -j"${ZJOBS:-4}" > build.log 2>&1 || { echo "  build FAILED"; tail -20 build.log; exit 1; }
    make install > install.log 2>&1
    cd ..
    echo "  done"
done

echo
echo "=== check: the result must be PIC, otherwise it cannot go into a PIE ==="
for a in "$ZPREFIX"/lib/libxcb-{util,image,keysyms,icccm,render-util,cursor}.a; do
    [ -f "$a" ] || { echo "  MISSING $a"; exit 1; }
    n=$(readelf -r "$a" 2>/dev/null | grep -cE 'R_X86_64_(32|32S)\b' || true)
    [ "$n" -eq 0 ] || { echo "  $(basename "$a"): NOT PIC ($n relocations)"; exit 1; }
    echo "  $(basename "$a"): PIC"
done
