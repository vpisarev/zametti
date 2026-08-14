#!/bin/bash
#
# Обновление вендоренного googletest. Запускать так:
#
#   git clone --depth 1 --branch v1.15.2 \
#       https://github.com/google/googletest /куда-нибудь
#   3rdparty/gtest/update.sh /куда-нибудь 3rdparty/gtest/upstream
#
# Берётся ТОЛЬКО googletest/ — сам фреймворк проверок. Выбрасываются:
#
#   googlemock/  — заглушки и ожидания вызовов. Нам не нужны: наборы проекта
#                  проверяют настоящий код на настоящих данных, а не разговор
#                  объекта с выдуманным соседом;
#   docs/, ci/   — документация и чужая сборочная обвязка;
#   BUILD.bazel, WORKSPACE*, *.bzl — сборка Bazel'ом, у нас CMake.
#
# Свои примеры и тесты самого gtest тоже уходят: чужие наборы нам гонять
# незачем, а весят они больше самого фреймворка.
#
# CMake пишем СВОЙ, а не берём чужой. Причина та же, что у md4c и blake3:
# у gtest в корне живёт option(BUILD_SHARED_LIBS), свои политики и установка,
# а исходников у него всего два файла-амальгамы. Проще перечислить их самим,
# чем разбираться, что ещё чужой CMakeLists положит нам в кэш.

set -e
SRC="$1"
DST="$2"
[ -d "$SRC" ] || { echo "нет исходного дерева: $SRC" >&2; exit 1; }
[ -n "$DST" ] || { echo "не сказано, куда класть" >&2; exit 1; }

rm -rf "$DST"
mkdir -p "$DST"
DST="$(cd "$DST" && pwd)"

cd "$SRC"
tar -cf - LICENSE README.md googletest/include googletest/src \
| (cd "$DST" && tar -xf -)

cd "$DST"
find . -name '.git*' -prune -exec rm -rf {} + 2>/dev/null || true
# Своих тестов и примеров gtest не приносим.
rm -rf googletest/test googletest/samples

echo "gtest: $(du -sh . | cut -f1), файлов $(find . -type f | wc -l)"
