#!/bin/bash
#
# Сборка помощников xcb статическими архивами с -fPIC в $ZPREFIX.
#
#   source packaging/linux/zenv.sh
#   packaging/linux/build-xcb-static.sh
#
# ЗАЧЕМ. Шесть библиотек, которые тянет xcb-плагин Qt, лежат в пакетах
# priority=EXTRA — самый низкий приоритет: в базовую установку они не входят
# никогда. libxcb-cursor0 из них — самая частая жалоба на Qt 6.5+ вообще
# («could not load the Qt platform plugin xcb»). Вшив их, мы снимаем шесть
# пакетов из требований к установке.
#
# ПОЧЕМУ ИЗ UPSTREAM, А НЕ АРХИВЫ ИЗ SYSROOT. Две причины, обе замеренные.
#
# Первая: Ubuntu собирает libxcb-image.a и libxcb-util.a БЕЗ -fPIC, и в
# PIE-программу их не вложить вовсе («relocation R_X86_64_32S ... can not be
# used when making a PIE object»).
#
# Вторая важнее. У libxcb-cursor встроен список каталогов, где искать темы
# курсоров, и он ИЗМЕНИЛСЯ:
#
#   focal:  ~/.icons:/usr/share/icons:/usr/share/pixmaps:/usr/X11R6/lib/X11/icons
#   ныне:   ~/.local/share/icons:~/.icons:/usr/share/icons:/usr/share/pixmaps
#
# Появился ~/.local/share/icons — ровно то место, куда GNOME и KDE кладут
# пользовательские темы сегодня. Вшив focal'ьную копию, мы получили бы
# программу, которая молча не видит тему курсора пользователя. Остальные пять
# — чистые счётные помощники: ни одного зашитого пути, публичный API не
# менялся, так что для них версия безразлична, и они берутся из upstream за
# компанию.

# ТРЕБОВАНИЯ К МАШИНЕ СБОРКИ (не к целевой): m4. Два пакета из шести —
# xcb-util-wm и xcb-util-cursor — генерируют им часть исходников, и без него
# их configure падает на пятом шаге сообщением, из которого причина не видна.
# Поэтому проверяем здесь и говорим прямо.

set -e
[ -n "$ZPREFIX" ] || { echo "нет ZPREFIX — сначала source packaging/linux/zenv.sh" >&2; exit 1; }
[ -n "$ZBUILD" ] || { echo "нет ZBUILD" >&2; exit 1; }
command -v m4 >/dev/null || {
    echo "нет m4 — xcb-util-wm и xcb-util-cursor генерируют им исходники." >&2
    echo "  sudo apt install m4" >&2
    exit 1
}

mkdir -p "$ZBUILD/xcb" && cd "$ZBUILD/xcb"

# Порядок важен: каждый следующий может зависеть от предыдущих.
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

    # Свои же, уже собранные, ищутся явными флагами, а не через PKG_CONFIG_PATH:
    # PKG_CONFIG_SYSROOT_DIR из zenv.sh приписал бы к путям $ZPREFIX ещё и
    # sysroot, и получилась бы чепуха вроде $ZSYS$ZPREFIX/include.
    own_cflags="-I$ZPREFIX/include"

    # --with-cursorpath ОБЯЗАТЕЛЕН, и вот почему. По умолчанию xcb-util-cursor
    # вшивает в код путь поиска тем курсоров, СОБРАННЫЙ ИЗ ${datadir}, то есть
    # из нашего --prefix. Без этого ключа в библиотеку попадало
    #
    #   ~/.local/share/icons:~/.icons:$ZPREFIX/share/icons:$ZPREFIX/share/pixmaps
    #
    # — программа искала бы системные курсоры в каталоге сборки, которого на
    # чужой машине нет вовсе. Замечено только тем, что после сборки посмотрели
    # `strings` готового архива; сборка при этом проходит молча и успешно.
    # Задаём путь целевой системы, а не свой.
    cursorpath='~/.local/share/icons:~/.icons:/usr/share/icons:/usr/share/pixmaps'

    ./configure --prefix="$ZPREFIX" --enable-static --disable-shared --with-pic \
        --with-cursorpath="$cursorpath" \
        CC="$CC" CFLAGS="-O2 -fPIC" \
        XCB_UTIL_CFLAGS="$own_cflags" XCB_UTIL_LIBS="-L$ZPREFIX/lib -lxcb-util" \
        XCB_IMAGE_CFLAGS="$own_cflags" XCB_IMAGE_LIBS="-L$ZPREFIX/lib -lxcb-image" \
        XCB_RENDERUTIL_CFLAGS="$own_cflags" XCB_RENDERUTIL_LIBS="-L$ZPREFIX/lib -lxcb-render-util" \
        > configure.log 2>&1 || { echo "  configure ПРОВАЛИЛСЯ"; tail -20 configure.log; exit 1; }
    make -j"${ZJOBS:-4}" > build.log 2>&1 || { echo "  сборка ПРОВАЛИЛАСЬ"; tail -20 build.log; exit 1; }
    make install > install.log 2>&1
    cd ..
    echo "  готово"
done

echo
echo "=== проверка: собранное должно быть PIC, иначе в PIE не вложить ==="
for a in "$ZPREFIX"/lib/libxcb-{util,image,keysyms,icccm,render-util,cursor}.a; do
    [ -f "$a" ] || { echo "  НЕТ $a"; exit 1; }
    n=$(readelf -r "$a" 2>/dev/null | grep -cE 'R_X86_64_(32|32S)\b' || true)
    [ "$n" -eq 0 ] || { echo "  $(basename "$a"): НЕ PIC ($n релокаций)"; exit 1; }
    echo "  $(basename "$a"): PIC"
done
