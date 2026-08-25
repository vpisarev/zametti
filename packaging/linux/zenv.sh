# Общее окружение переносимых сборок zametti под Linux. Подключать так:
#
#   source packaging/linux/zenv.sh
#
# Смысл: все чужие библиотеки и сама программа собираются ОДНИМИ И ТЕМИ ЖЕ
# ключами против sysroot Ubuntu 20.04. Разошедшиеся флаги у Qt и у программы —
# это классический способ получить бинарь, который собрался, но не запускается.
#
# Пути можно задать снаружи (`ZSYS=/иное/место source packaging/linux/zenv.sh`);
# без этого берутся привычные. Раньше файл лежал в самом sysroot
# (`~/work/zsys/bin/zenv.sh`) и переезжал вместе с ним; теперь он в
# репозитории — чтобы рецепт из docs/zametti-build-linux.md воспроизводился на
# новой машине целиком, а не наполовину.

export ZSYS="${ZSYS:-$HOME/work/zsys}"        # sysroot (заменяется распаковкой тарбола)
export ZPREFIX="${ZPREFIX:-$HOME/work/zdeps}" # куда кладём собранное (тарболом НЕ затирается)
export ZBUILD="${ZBUILD:-$HOME/work/zbuild}"  # где собираем

# Обёртки, а не сам gcc-15: они дописывают --sysroot и LD_LIBRARY_PATH к
# focal'ьным hostlibs. Их исходник — packaging/linux/sysroot-bin/, а в
# $ZSYS/bin их кладёт packaging/linux/fix-sysroot.sh.
export CC="$ZSYS/bin/gcc"
export CXX="$ZSYS/bin/g++"

# pkg-config смотрит ТОЛЬКО в sysroot: LIBDIR (а не PATH) отрезает системные
# каталоги хоста, иначе нашлись бы .pc от свежей Ubuntu. usr/share/pkgconfig
# обязателен — там лежит wayland-protocols.pc.
export PKG_CONFIG_SYSROOT_DIR="$ZSYS"
export PKG_CONFIG_LIBDIR="$ZSYS/usr/lib/x86_64-linux-gnu/pkgconfig:$ZSYS/usr/share/pkgconfig"

# -fPIC обязателен: статические библиотеки уедут внутрь PIE-программы.
export ZCFLAGS="-O2 -fPIC"
# Обязательны, а не желательны: libstdc++ у gcc-15 требует GLIBCXX_3.4.32,
# которого нет даже в той Ubuntu 20.04, откуда снят sysroot.
export ZLDFLAGS="-static-libstdc++ -static-libgcc"

if [ ! -x "$CC" ]; then
    echo "zenv: нет $CC — сперва подготовьте sysroot:" >&2
    echo "        bash packaging/linux/fix-sysroot.sh" >&2
fi
echo "zenv: sysroot=$ZSYS prefix=$ZPREFIX cc=$(basename "$CC")"
