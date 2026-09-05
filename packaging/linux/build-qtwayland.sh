#!/bin/bash
#
# Build the qtwayland module against the static Qt already installed in $ZPREFIX.
#
#   git -C $ZBUILD clone --depth 1 --branch v6.10.3 https://code.qt.io/qt/qtwayland.git
#   packaging/linux/build-qtwayland.sh
#
# WHY IT IS NEEDED AT ALL WHEN WAYLAND ALREADY WORKS. In Qt 6.10 the wayland
# CLIENT moved into qtbase, so the program already runs under Wayland without
# this module. What stays in qtwayland is the `adwaita` decoration plugin:
# the GNOME window title bar.
#
# Under Wayland the title bar is drawn by the program itself, not by the window
# manager, and Qt picks how: under GNOME it looks for a plugin keyed
# `adwaita`/`gnome` and, NOT FINDING one, takes the first available, i.e. its
# own fallback `bradient` with a grey-blue gradient (qtbase, qwaylandwindow.cpp,
# createDecoration). Measured: under XDG_CURRENT_DESKTOP=ubuntu:GNOME the
# window requests geometry (10, 10, 1152, 820), which is adwaita with its
# shadows; under KDE and with an empty value it requests (0, 0, 1156, 813),
# i.e. bradient.
#
# The plugin brings NOT A SINGLE new dynamic dependency: it asks for Qt::DBus,
# Qt::Svg and Wayland::Client, and libdbus-1 and Qt6Svg are already in the
# program. The NEEDED list after adding it matched character for character.
#
# THE TITLE BAR FONT PATCH lives in packaging/linux/patches/. The plugin takes
# its font from the platform theme (`theme->font(QPlatformTheme::TitleBarFont)`),
# and QGnomeTheme returns nullptr for it; only QGtk3Theme knows the right font,
# and we deliberately do not pull in gtk3 (that is +12 dynamic dependencies).
# The fallback `QFont("Cantarell", 10)` kicks in and the title comes out smaller
# than GNOME's: GNOME asks for `Adwaita Sans Bold 11`. Yet the plugin ALREADY
# REQUESTS THAT STRING FROM THE PORTAL and uses exactly one word of it, "bold".
# The patch takes all of it. Checked against newer versions: 6.11.2 and dev
# carry both problems word for word, so upgrading Qt would not help.
#
# SCANNER FROM THE SYSROOT, NOT FROM THE HOST. The host wayland-scanner (1.24)
# generates code calling wl_proxy_marshal_flags, while the Ubuntu 20.04 sysroot
# has libwayland 1.18, which lacks that symbol. Same switch as for qtbase.
set -e

# WE SET UP THE ENVIRONMENT OURSELVES: no need to `source zenv.sh` before this
# script (values set outside are left alone: ZSYS=/other/place works as before).
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$HERE/zenv.sh"

SRC="$ZBUILD/qtwayland"
BUILD="$ZBUILD/qtwayland-build"
PATCHES="$(cd "$(dirname "${BASH_SOURCE[0]}")/patches" && pwd)"

[ -d "$SRC" ] || { echo "no qtwayland tree at $SRC" >&2; exit 1; }
[ -x "$ZPREFIX/bin/qt-cmake" ] || { echo "no $ZPREFIX/bin/qt-cmake — build qtbase first" >&2; exit 1; }

# The patch is applied exactly once and silently skipped if already present.
cd "$SRC"
for patch in "$PATCHES"/qtwayland-*.patch; do
    [ -e "$patch" ] || continue
    if git apply --reverse --check "$patch" 2>/dev/null; then
        echo "already applied: $(basename "$patch")"
    else
        git apply "$patch"
        echo "applied: $(basename "$patch")"
    fi
done

mkdir -p "$BUILD"
cd "$BUILD"
"$ZPREFIX/bin/qt-cmake" "$SRC" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$ZPREFIX" \
    -DQT_BUILD_EXAMPLES=OFF \
    -DQT_BUILD_TESTS=OFF \
    -DWaylandScanner_EXECUTABLE="$ZSYS/usr/bin/wayland-scanner"

# The summary must say "GNOME-like client-side decorations ... yes": without
# Qt::DBus or Qt::Svg the feature condition fails and the plugin silently is
# not built.
grep -q "GNOME-like client-side decorations ... yes" config.summary || {
    echo "qtwayland configured WITHOUT the adwaita plugin — check that $ZPREFIX has Qt6 DBus and Svg" >&2
    exit 1
}

cmake --build . -j8
cmake --install .

test -f "$ZPREFIX/plugins/wayland-decoration-client/libadwaita.a" || {
    echo "libadwaita.a was not installed" >&2; exit 1; }
echo "qtwayland: done, adwaita in $ZPREFIX/plugins/wayland-decoration-client/"
