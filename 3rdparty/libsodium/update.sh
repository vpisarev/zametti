#!/bin/bash
#
# Обновление вендоренного libsodium. Запускать так:
#
#   git clone --depth 1 --branch 1.0.20-RELEASE \
#       https://github.com/jedisct1/libsodium /куда-нибудь
#   3rdparty/libsodium/update.sh /куда-нибудь 3rdparty/libsodium/upstream
#
# ПОЧЕМУ ВЕНДОРИНГ. Раньше libsodium была единственной жёсткой СИСТЕМНОЙ
# зависимостью помимо Qt, и на чистой машине сборка требовала отдельного
# `apt install libsodium-dev`. Для переносимой статической сборки его всё
# равно пришлось бы собирать самим — против sysroot, а не из системы. Раз так,
# вендоринг убирает шаг развёртки и ничего не добавляет.
#
# ПОЧЕМУ СВОЙ CMake, А НЕ ЧУЖОЙ. Чужого нет вовсе: upstream собирается
# autotools, а под Windows — отдельными проектами MSVC. Свой CMake здесь не
# прихоть, а единственный способ; прецедент в дереве есть (blake3, zlib, zstd,
# md4c, dtl, jpegli — у них тоже наш).
#
# ЧТО БЕРЁТСЯ. Только `src/libsodium` — это вся библиотека целиком, 119 файлов
# .c плюс заголовки и два настоящих .S. Плюс LICENSE и configure.ac: из
# последнего наш CMake достаёт номер версии, чтобы он не разъехался с деревом
# при следующем обновлении (вписанная руками версия разъезжается всегда), а
# ещё оттуда дословно взяты тела двух asm-проб.
#
# ЧТО ВЫБРАСЫВАЕТСЯ: test/ (4.2 МБ — вчетверо тяжелее кода), m4/, build-aux/,
# dist-build/, ci/, packaging/, regen-msvc/, builds/, contrib/ и файлы
# autotools (Makefile.am, Makefile.in) внутри дерева — их наш CMake, в отличие
# от libwebp, не читает.
#
# ФАЙЛЫ .S НУЖНЫ, И ОНИ ОСТАЮТСЯ. Соблазн выбросить их велик (gas-синтаксис,
# MSVC их не возьмёт), но выбросить нельзя: макрос HAVE_AVX_ASM управляет не
# только ускорением curve25519, а ещё и ЧТЕНИЕМ РЕГИСТРА XCR0 в runtime.c —
# без него libsodium решает, что ОС не разрешила AVX, и отключает все пути
# AVX/AVX2 на исполнении. Молча. Подробности и страж — в CMakeLists.txt рядом.
#
# Настоящих исходников среди .S два: salsa20_xmm6-asm.S и sandy2x.S; остальные
# .S в sandy2x включаются внутрь второго и отдельно не собираются.

set -e
SRC="$1"
DST="$2"
[ -d "$SRC" ] || { echo "нет исходного дерева: $SRC" >&2; exit 1; }
[ -n "$DST" ] || { echo "не сказано, куда класть" >&2; exit 1; }

rm -rf "$DST"
mkdir -p "$DST"
DST="$(cd "$DST" && pwd)"

cd "$SRC"
tar -cf - LICENSE configure.ac src/libsodium | (cd "$DST" && tar -xf -)

cd "$DST"
find . -name '.git*' -prune -exec rm -rf {} + 2>/dev/null || true
find . -name 'Makefile.am' -delete
find . -name 'Makefile.in' -delete

echo "libsodium: $(du -sh . | cut -f1), файлов $(find . -type f | wc -l)"
