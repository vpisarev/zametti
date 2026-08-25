#!/bin/bash
#
# Обновление вендоренного libgav1. Скачивать так (ВНИМАНИЕ, НЕ КАК ОСТАЛЬНЫЕ):
#
#   mkdir /куда-нибудь && cd /куда-нибудь
#   curl -sL "https://chromium.googlesource.com/codecs/libgav1/+archive/refs/tags/v0.20.0.tar.gz" \
#       | tar xz
#   3rdparty/libgav1/update.sh /куда-нибудь 3rdparty/libgav1/upstream
#
# Архив googlesource раскладывается БЕЗ ВЕРХНЕГО КАТАЛОГА — содержимое ложится
# прямо в текущий. Скрипт, написанный по образцу github-архива, распакует
# дерево на уровень выше и снесёт соседей.
#
# ЗАЧЕМ ОН НАМ. Это декодер AV1 — то, чем разжимается AVIF. libheif контейнер
# разбирает, а кодек зовёт со стороны.
#
# ПОЧЕМУ libgav1, А НЕ libaom (решение владельца 25.08.2026). У libaom 24 файла
# ассемблера, и единственный способ собрать его без ассемблера —
# AOM_TARGET_CPU=generic, который гасит ЗАОДНО И ИНТРИНЗИКИ: замер на
# avif-sample.avif дал 0.13 с против 0.30 с, то есть 2.3 раза. У libgav1 файлов
# ассемблера НОЛЬ, а вся векторизация — интринзиками (SSE4.1, AVX2, NEON),
# которые компилятор берёт сам. Плюс дерево вдвое с лишним меньше: 5.5 МБ
# против 12.3 МБ обрезанного libaom.
#
# ПОЧЕМУ ЧУЖОЙ CMake, А НЕ СВОЙ СПИСОК ФАЙЛОВ. Списки у libgav1 чистые, и свой
# CMakeLists написать легко — но раздача пофайловых ключей у него по суффиксу
# имени (cmake/libgav1_intrinsics.cmake: *_sse4.cc получают -msse4.1, *_avx2.cc
# получают -mavx2). А в src/dsp/x86/cdef_sse4.cc под #else лежит ПУСТАЯ
# ЗАГЛУШКА: файл без ключа компилируется в ничто. Забыть ключ на одном файле из
# 22 — значит собрать молча более медленную библиотеку, ровно тот отказ, ради
# которого мы и ушли от libaom.
#
# ЧТО БЕРЁТСЯ:
#   CMakeLists.txt, LICENSE, AUTHORS, cmake/, src/;
#   examples/libgav1_examples.cmake и tests/libgav1_tests.cmake — ДВА ФАЙЛА В
#   ПУСТЫХ КАТАЛОГАХ, И ОНИ ОБЯЗАТЕЛЬНЫ. Корневой CMakeLists.txt подключает их
#   БЕЗУСЛОВНО (строки 71 и 81), безо всякого if. Выбросить каталоги целиком —
#   значит сломать чужую настройку на include() несуществующего файла. Сами по
#   себе они безвредны: при выключенных LIBGAV1_ENABLE_EXAMPLES/TESTS каждый
#   определяет пустой макрос и делает return().
#
# ЧТО ВЫБРАСЫВАЕТСЯ: 59 тестовых файлов внутри src/ (в списках исходников
# библиотеки они не значатся — проверено, ноль совпадений на «_test» в
# src/libgav1_decoder.cmake, src/dsp/libgav1_dsp.cmake и
# src/utils/libgav1_utils.cmake), исходники примеров, tests/data и
# tests/third_party, cmake/toolchains (чужие тулчейны, у нас свои в
# packaging/), проза и настройки чужого CI.
#
# АССЕМБЛЕРА НЕТ НИ ОДНОГО ФАЙЛА — это и есть причина выбора. Если после
# обновления `find upstream -name '*.S' -o -name '*.asm'` что-то найдёт,
# основание вендоринга изменилось, и это повод остановиться и подумать.
set -e
SRC="$1"
DST="$2"
[ -d "$SRC" ] || { echo "нет исходного дерева: $SRC" >&2; exit 1; }
[ -n "$DST" ] || { echo "не сказано, куда класть" >&2; exit 1; }
[ -f "$SRC/CMakeLists.txt" ] || { echo "в $SRC нет CMakeLists.txt — архив googlesource распаковывается БЕЗ верхнего каталога, проверьте путь" >&2; exit 1; }

rm -rf "$DST"
mkdir -p "$DST"
DST="$(cd "$DST" && pwd)"

cd "$SRC"
tar -cf - CMakeLists.txt LICENSE AUTHORS cmake src \
          examples/libgav1_examples.cmake tests/libgav1_tests.cmake \
    | (cd "$DST" && tar -xf -)

cd "$DST"
find . -name '.git*' -prune -exec rm -rf {} + 2>/dev/null || true
find src \( -name '*_test.cc' -o -name '*_test.h' -o -name '*_test.c' \
         -o -name '*_test_data.h' -o -name '*_test_data.inc' \) -delete
rm -rf cmake/toolchains

asm=$(find . \( -name '*.S' -o -name '*.asm' \) | wc -l)
if [ "$asm" != "0" ]; then
    echo "ВНИМАНИЕ: в дереве $asm файлов ассемблера, а их не должно быть ни одного." >&2
fi

echo "libgav1: $(du -sh . | cut -f1), файлов $(find . -type f | wc -l), ассемблера $asm"
