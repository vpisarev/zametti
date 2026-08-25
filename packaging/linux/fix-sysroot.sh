#!/bin/bash
#
# Подготовка sysroot после распаковки zsys.tar. ЗАПУСКАТЬ КАЖДЫЙ РАЗ после
# `tar -xf zsys.tar` — архив приносит абсолютные симлинки заново.
#
#   bash packaging/linux/fix-sysroot.sh            # sysroot берётся из $ZSYS
#   bash packaging/linux/fix-sysroot.sh /иной/путь
#
# Делает две вещи.
#
# 1. ЧИНИТ СИМЛИНКИ. В Ubuntu dev-симлинки вида libpthread.so указывают
#    АБСОЛЮТНЫМ путём на /lib/x86_64-linux-gnu/libpthread.so.0. Внутри sysroot
#    такой путь разрешает не линковщик, а ядро — и разрешает его от НАСТОЯЩЕГО
#    корня, то есть от хоста. В итоге -lpthread, -ldl, -lrt, -luuid и родня
#    тихо брались бы из glibc хоста вместо sysroot, и переносимость утекала бы
#    без единого сообщения. Там, где на хосте нужной версии не оказалось
#    (libdbus-1), симлинк просто битый и линковщик падает — это как раз
#    везение, а не норма.
#
#    Чиним переписыванием в ОТНОСИТЕЛЬНЫЙ путь: он разрешается внутри sysroot и
#    от хоста больше не зависит. Трогаем только те, чья цель в sysroot есть;
#    остальные (aspell в /var/lib, /etc/alternatives) оставляем как есть — они
#    и так мусор, и к сборке отношения не имеют.
#
# 2. СТАВИТ ОБЁРТКИ КОМПИЛЯТОРА в $ZSYS/bin. Их исходник — sysroot-bin/ рядом
#    с этим файлом, то есть в репозитории; в sysroot они попадают копией.
#    Раньше они жили только в sysroot, и рецепт сборки на новой машине
#    воспроизводился наполовину. Обёртки вычисляют корень от собственного
#    расположения, поэтому работают ровно там, куда положены, и нигде больше —
#    зовите их по пути $ZSYS/bin/gcc, а не отсюда.
set -e

ROOT="${1:-$ZSYS}"
if [ -z "$ROOT" ]; then
    echo "не задан sysroot: ни аргументом, ни в \$ZSYS" >&2
    echo "  source packaging/linux/zenv.sh" >&2
    exit 1
fi
ROOT="$(cd "$ROOT" && pwd)"
[ -d "$ROOT/usr/include" ] || { echo "$ROOT не похож на sysroot: нет usr/include" >&2; exit 1; }

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
echo "  симлинков переписано в относительные: $fixed"
echo "  оставлено как есть (цели нет в sysroot): $skipped"
echo "  обёртки компилятора: $ROOT/bin/{gcc,g++}"

# hostlibs — четыре библиотеки focal (libisl, libbfd, libopcodes), без которых
# focal'ьный gcc-15 не запускается на хосте. Двоичные файлы, в репозитории им
# не место; но молчать о пропаже нельзя — без них обёртка падает при первом же
# вызове, и сообщение линковщика причины не назовёт.
if [ ! -d "$ROOT/hostlibs" ]; then
    echo "  ВНИМАНИЕ: нет $ROOT/hostlibs — gcc-15 из sysroot не запустится на хосте." >&2
    echo "            Туда кладутся libisl/libbfd/libopcodes из той же Ubuntu 20.04." >&2
fi
"$ROOT/bin/gcc" --version >/dev/null || { echo "  обёртка не запускается" >&2; exit 1; }
