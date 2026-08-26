#!/bin/bash
#
# Сборка модуля qtwayland против уже собранного статического Qt в $ZPREFIX.
#
#   source packaging/linux/zenv.sh
#   git -C $ZBUILD clone --depth 1 --branch v6.10.3 https://code.qt.io/qt/qtwayland.git
#   packaging/linux/build-qtwayland.sh
#
# ЗАЧЕМ ОН ВООБЩЕ НУЖЕН, ЕСЛИ WAYLAND И ТАК РАБОТАЕТ. В Qt 6.10 wayland-КЛИЕНТ
# переехал в qtbase, и без этого модуля программа под Wayland уже живёт. Но в
# qtwayland остался decoration-плагин `adwaita` — гномовский заголовок окна.
#
# Под Wayland заголовок рисует не оконный менеджер, а сама программа, и Qt
# выбирает чем: под GNOME она ищет плагин с ключом `adwaita`/`gnome` и, НЕ
# НАЙДЯ, берёт первый попавшийся — то есть свой запасной `bradient` с
# серо-синим градиентом (qtbase, qwaylandwindow.cpp, createDecoration).
# Проверено замером: под XDG_CURRENT_DESKTOP=ubuntu:GNOME окно просит
# геометрию (10, 10, 1152, 820) — это adwaita с её тенями, — а под KDE и с
# пустым значением (0, 0, 1156, 813), то есть bradient.
#
# Новых динамических зависимостей плагин не приносит НИ ОДНОЙ: он просит
# Qt::DBus, Qt::Svg и Wayland::Client, а libdbus-1 и Qt6Svg в программе уже
# есть. Список NEEDED после его добавления совпал посимвольно.
#
# ПАТЧ ПРО ШРИФТ ЗАГОЛОВКА — packaging/linux/patches/. Плагин берёт шрифт у
# платформенной темы (`theme->font(QPlatformTheme::TitleBarFont)`), а
# QGnomeTheme отдаёт для него nullptr — правильный шрифт умеет только
# QGtk3Theme, а gtk3 мы нарочно не тянем (это +12 динамических зависимостей).
# Срабатывает запасной `QFont("Cantarell", 10)`, и заголовок выходит мельче
# гномовского: GNOME просит `Adwaita Sans Bold 11`. Причём плагин ЭТУ СТРОКУ
# У ПОРТАЛА УЖЕ ЗАПРАШИВАЕТ и использует из неё одно слово «bold». Патч берёт
# из неё всё. Проверено в свежих версиях: в 6.11.2 и в dev обе беды на месте
# слово в слово, обновление Qt не помогло бы.
#
# СКАНЕР — ИЗ SYSROOT, А НЕ С ХОСТА. wayland-scanner хоста (1.24) порождает
# код, зовущий wl_proxy_marshal_flags, а в sysroot Ubuntu 20.04 libwayland
# 1.18, где такого символа нет. Тот же ключ, что у qtbase.
set -e

: "${ZBUILD:?не задан ZBUILD — сперва source packaging/linux/zenv.sh}"
: "${ZPREFIX:?не задан ZPREFIX}"
: "${ZSYS:?не задан ZSYS}"

SRC="$ZBUILD/qtwayland"
BUILD="$ZBUILD/qtwayland-build"
PATCHES="$(cd "$(dirname "${BASH_SOURCE[0]}")/patches" && pwd)"

[ -d "$SRC" ] || { echo "нет дерева qtwayland в $SRC" >&2; exit 1; }
[ -x "$ZPREFIX/bin/qt-cmake" ] || { echo "нет $ZPREFIX/bin/qt-cmake — сперва соберите qtbase" >&2; exit 1; }

# Патч накладывается ровно один раз и молча пропускается, если уже лежит.
cd "$SRC"
for patch in "$PATCHES"/qtwayland-*.patch; do
    [ -e "$patch" ] || continue
    if git apply --reverse --check "$patch" 2>/dev/null; then
        echo "уже наложен: $(basename "$patch")"
    else
        git apply "$patch"
        echo "наложен: $(basename "$patch")"
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

# Сводка обязана сказать «GNOME-like client-side decorations ... yes»: без
# Qt::DBus или Qt::Svg условие фичи не выполняется, и плагин молча не соберётся.
grep -q "GNOME-like client-side decorations ... yes" config.summary || {
    echo "qtwayland настроен БЕЗ плагина adwaita — проверьте, что в $ZPREFIX есть Qt6 DBus и Svg" >&2
    exit 1
}

cmake --build . -j8
cmake --install .

test -f "$ZPREFIX/plugins/wayland-decoration-client/libadwaita.a" || {
    echo "libadwaita.a не установилась" >&2; exit 1; }
echo "qtwayland: готово, adwaita в $ZPREFIX/plugins/wayland-decoration-client/"
