# Общее окружение кросс-сборки zametti под Windows. Подключать так:
#
#   source packaging/win/zenv.sh
#
# Смысл тот же, что у packaging/linux/zenv.sh: и Qt, и чужие библиотеки, и сама
# программа собираются ОДНИМ компилятором с ОДНИМИ ключами. Здесь это mingw-w64
# с хоста — целевой мир (/usr/x86_64-w64-mingw32) с хостовым не пересекается,
# поэтому sysroot не нужен, а планку задаёт _WIN32_WINNT из toolchain'а.
#
# Пути задаются снаружи (`ZWPREFIX=/иное/место source packaging/win/zenv.sh`).

export ZWTRIPLE="${ZWTRIPLE:-x86_64-w64-mingw32}"
export ZWPREFIX="${ZWPREFIX:-$HOME/work/zwin}"    # куда кладём собранное (Qt)
export ZWBUILD="${ZWBUILD:-$HOME/work/zbuild}"    # где собираем (общий с linux)

# Хостовая Qt ТОЙ ЖЕ версии — из неё кросс-сборка берёт moc, rcc, uic и syncqt.
# Берём статическую сборку переносимого Linux-рецепта: версия там 6.10.3, а
# инструменты в libexec/ — обычные хостовые программы, статичность им не мешает.
export QT_HOST_PATH="${QT_HOST_PATH:-$HOME/work/zdeps}"

export ZWTOOLCHAIN="${ZWTOOLCHAIN:-$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/toolchains/mingw-w64.cmake}"

export CC="${ZWTRIPLE}-gcc"
export CXX="${ZWTRIPLE}-g++"

# pkg-config под цель: mingw кладёт свои .pc туда же, куда библиотеки.
# LIBDIR (а не PATH) — чтобы хостовые .pc не были видны вовсе.
export PKG_CONFIG_LIBDIR="/usr/${ZWTRIPLE}/lib/pkgconfig:$ZWPREFIX/lib/pkgconfig"
unset PKG_CONFIG_SYSROOT_DIR

if ! command -v "$CC" >/dev/null 2>&1; then
    echo "zenv-win: нет $CC — поставьте mingw-w64:" >&2
    echo "          sudo apt install mingw-w64" >&2
fi
if [ ! -x "$QT_HOST_PATH/libexec/moc" ]; then
    echo "zenv-win: в QT_HOST_PATH=$QT_HOST_PATH нет libexec/moc —" >&2
    echo "          кросс-сборке Qt нечем будет обработать заголовки" >&2
fi
echo "zenv-win: triple=$ZWTRIPLE prefix=$ZWPREFIX host-qt=$QT_HOST_PATH"
