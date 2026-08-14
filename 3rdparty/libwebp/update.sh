#!/bin/bash
#
# Обновление вендоренного libwebp. Запускать так:
#
#   git clone --depth 1 --branch v1.5.0 \
#       https://chromium.googlesource.com/webm/libwebp /куда-нибудь
#   3rdparty/libwebp/update.sh /куда-нибудь 3rdparty/libwebp/upstream
#
# Берётся ТОЛЬКО то, без чего не собрать декодер: сам код (src, sharpyuv),
# сборочные файлы (CMakeLists.txt, cmake) и configure.ac, из которого чужой
# CMake достаёт номер версии. Выбрасываются утилиты командной строки, примеры,
# тесты, extras, привязки swig, сборка под Emscripten, Android и iOS,
# документация — вместе они втрое тяжелее кода и нам не нужны ни строкой.
#
# ВНИМАНИЕ: файлы Makefile.am внутри src/ и sharpyuv/ выбрасывать нельзя,
# хотя автотулзами мы не собираем. Чужой CMake читает списки исходников
# ИМЕННО ИЗ НИХ (функция parse_Makefile_am), и без них не соберётся ничего.
#
# CMake у libwebp берём ЧУЖОЙ, а не пишем свой, по той же причине, что у
# libtiff: он сам разбирается, какие расширения процессора доступны, и каждому
# файлу SIMD-диспетчера даёт свои ключи (-msse4.1, -mavx2, -mfpu=neon). Писать
# это руками — заново делать чужую работу и ошибаться в ней на каждой новой
# машине.
#
# Нам нужен ТОЛЬКО ДЕКОДЕР. Энкодер webp не нужен нигде: живой путь ввоза
# картинки один, и он кладёт JPEG XL. Цели webp, webpdemux и webpmux у чужого
# CMake остаются описанными, но под EXCLUDE_FROM_ALL не собираются никогда.

set -e
SRC="$1"
DST="$2"
[ -d "$SRC" ] || { echo "нет исходного дерева: $SRC" >&2; exit 1; }
[ -n "$DST" ] || { echo "не сказано, куда класть" >&2; exit 1; }

rm -rf "$DST"
mkdir -p "$DST"
DST="$(cd "$DST" && pwd)"

cd "$SRC"
tar -cf - \
  CMakeLists.txt configure.ac \
  COPYING PATENTS AUTHORS README.md \
  cmake src sharpyuv \
| (cd "$DST" && tar -xf -)

cd "$DST"
find . -name '.git*' -prune -exec rm -rf {} + 2>/dev/null || true

echo "libwebp: $(du -sh . | cut -f1), файлов $(find . -type f | wc -l)"
