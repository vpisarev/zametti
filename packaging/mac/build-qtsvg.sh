#!/bin/bash
#
# qtsvg против статического Qt из $ZPREFIX. Отдельным файлом, потому что это
# отдельный репозиторий Qt, а не часть qtbase.
#
#   git -C $ZBUILD clone --depth 1 --branch v6.10.3 https://code.qt.io/qt/qtsvg.git
#   bash packaging/mac/build-qtsvg.sh
#
# ЗАЧЕМ ОН НУЖЕН. Иконки тулбара — Lucide, то есть SVG. Программа линкует
# Qt6::Svg МОДУЛЕМ, а не полагается на плагин imageformats: плагин лежит в
# необязательном пакете, и на машине без него тулбар оказался бы пуст молча
# (довод записан в app/CMakeLists.txt). В статической сборке разницы «пакет
# есть / пакета нет» не существует вовсе: модуль уезжает внутрь бинаря.
#
# Собирается ключами САМОГО Qt: qt-cmake подставляет их из $ZPREFIX, поэтому
# ни -static, ни версию системы здесь повторять не надо — и разойтись им негде.
set -e

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=zenv.sh
source "$HERE/zenv.sh"

[ -x "$ZPREFIX/bin/qt-cmake" ] || {
    echo "нет $ZPREFIX/bin/qt-cmake — сперва соберите qtbase:" >&2
    echo "        bash packaging/mac/qtbase-configure.sh" >&2
    exit 1
}
[ -d "$ZBUILD/qtsvg" ] || { echo "нет дерева qtsvg в $ZBUILD/qtsvg" >&2; exit 1; }

mkdir -p "$ZBUILD/qtsvg-build"
cd "$ZBUILD/qtsvg-build"
"$ZPREFIX/bin/qt-cmake" "$ZBUILD/qtsvg" -DCMAKE_BUILD_TYPE=Release
cmake --build . -j8
cmake --install .
