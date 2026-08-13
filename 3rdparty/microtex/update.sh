#!/bin/sh
# Обновление вендоренной копии MicroTeX.
#
# Откуда:  https://github.com/NanoMichael/MicroTeX
# Ветка:   openmath  (НЕ master!)
# Коммит:  086f4eb740270b28bd0c61a0a359aea9300d61ae, 2024-08-05
#
# Почему openmath, а не master. Это две разные библиотеки в одном репозитории.
# master — старый порт JLaTeXMath: метрики шрифтов зашиты в 40 сгенерированных
# .def.cpp, растяжимых глифов из шрифта он не берёт, и вдобавок требует
# tinyxml2, которого в этой системе нет, а поставить нечем (нет sudo).
# openmath — MicroTeX 1.0 с настоящей поддержкой OpenType MATH: скобки,
# \widehat и \overbrace собираются из glyph assembly шрифта. Ровно то, по чему
# бриф отбирает кандидата.
#
# Что выброшено и почему (решение владельца — «бери только ядро и рисовалку
# для qt»):
#   platform/{cairo,gtk,skia,wasm,flutter,gdi_win} — нам нужен только Qt;
#   res/ целиком — бандл шрифтов XITS (4,4 МБ) не нужен: свои гарнитуры
#     (Euler Math + Latin Modern) лежат в resources/fonts/math и уезжают в qrc;
#   prebuilt/ — генератор .clm2 нужен только при пересборке ресурсов, и он
#     описан в resources/fonts/math/README.md;
#   example/, test/, readme/, doc/, .github/, githooks/, build.zig, meson*,
#     clatexmath.wrap, vcpkg.json — чужая обвязка.
#
# Использование:  ./update.sh [куда]      (по умолчанию — ./upstream)

set -e

REPO=https://github.com/NanoMichael/MicroTeX
COMMIT=086f4eb740270b28bd0c61a0a359aea9300d61ae
BRANCH=openmath

DST=$(cd "$(dirname "$0")" && pwd)/${1:-upstream}
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

echo "клонирую $REPO ($BRANCH)"
git clone --quiet --branch "$BRANCH" "$REPO" "$TMP/src"
git -C "$TMP/src" checkout --quiet "$COMMIT"

echo "полное дерево: $(du -sh "$TMP/src" | cut -f1), файлов $(find "$TMP/src" -type f -not -path '*/.git/*' | wc -l)"

rm -rf "$DST"
mkdir -p "$DST"

# Явный список того, что переезжает. Всё, чего здесь нет, остаётся за бортом.
(cd "$TMP/src" && tar -cf - \
    CMakeLists.txt \
    MicroTeXInstall.cmake \
    LICENSE \
    lib \
    platform/qt \
) | (cd "$DST" && tar -xf -)

# На всякий случай: никаких .git* внутри вендоренного дерева.
find "$DST" -name '.git*' -prune -exec rm -rf {} +

echo "$COMMIT" > "$DST/.vendored-commit"

# Патчи держим отдельными файлами: так видно, что именно правлено в чужом
# коде, и переезд на новый коммит остаётся осознанным.
for p in "$(dirname "$0")"/patches/*.patch; do
    [ -e "$p" ] || continue
    echo "накладываю $(basename "$p")"
    patch -p1 -d "$DST" < "$p"
done

echo "microtex: $(du -sh "$DST" | cut -f1), файлов $(find "$DST" -type f | wc -l)"
echo "  исходники lib+platform: $(find "$DST/lib" "$DST/platform" -name '*.cpp' -o -name '*.h' | wc -l) файлов, \
$(cat $(find "$DST/lib" "$DST/platform" -name '*.cpp' -o -name '*.h') | wc -l) строк"
