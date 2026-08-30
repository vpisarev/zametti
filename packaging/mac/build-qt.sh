#!/bin/bash
#
# Статический Qt под macOS целиком: заплаты + configure + сборка + установка
# qtbase, затем qtsvg — для каждой архитектуры из $ZARCHS по очереди.
#
#   git -C $ZBUILD clone --depth 1 --branch v6.10.3 https://code.qt.io/qt/qtbase.git
#   git -C $ZBUILD clone --depth 1 --branch v6.10.3 https://code.qt.io/qt/qtsvg.git
#   bash packaging/mac/build-qt.sh
#
# Раньше настройка и сборка были разными файлами — защита от ручного
# `cmake --build . | tail`, где код возврата приходит от tail и упавшая сборка
# рапортует нулём. Здесь пайпов нет, set -e -o pipefail останавливает скрипт на
# первом же ненулевом шаге, так что нужды в разделении не осталось.
#
# Версия та же, что у переносимой сборки под линукс, — 6.10.3 (слово владельца):
# два разных Qt на двух системах означали бы два разных набора багов вёрстки, а
# сравнивать снимки приёмки надо с одним и тем же.
#
# ЗАЧЕМ qtsvg. Иконки тулбара — Lucide, то есть SVG. Программа линкует Qt6::Svg
# МОДУЛЕМ, а не полагается на плагин imageformats: плагин лежит в необязательном
# пакете, и на машине без него тулбар оказался бы пуст молча (довод записан в
# app/CMakeLists.txt). В статической сборке модуль просто уезжает внутрь бинаря.
#
# Как читать список ключей configure:
#   -qt-      своя копия дешевле лишней динамической зависимости;
#   -no-      вещь тянет за собой мир (glib, ICU) или не нужна вовсе (CUPS — на
#             бумагу мы выводим своим PDF).
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=zenv.sh
source "$HERE/zenv.sh"

[ -d "$ZBUILD/qtbase" ] || { echo "нет дерева qtbase в $ZBUILD/qtbase (клонирование — в шапке скрипта)" >&2; exit 1; }
[ -d "$ZBUILD/qtsvg" ] || { echo "нет дерева qtsvg в $ZBUILD/qtsvg (клонирование — в шапке скрипта)" >&2; exit 1; }

# ЗАПЛАТЫ К QT. Каждая — с объяснением внутри самой заплаты; переносить их при
# смене версии Qt обязательно, и проверять, не починено ли наверху.
# Накладываются идемпотентно (--check перед --apply) и ОДИН раз на дерево:
# дерево исходников общее, арх-зависимого в заплатах нет, собираются обе
# архитектуры из него out-of-source.
for patch in "$HERE"/patches/qtbase-*.patch; do
    [ -e "$patch" ] || continue
    if git -C "$ZBUILD/qtbase" apply --check "$patch" 2>/dev/null; then
        git -C "$ZBUILD/qtbase" apply "$patch"
        echo "заплата наложена: $(basename "$patch")"
    else
        echo "заплата уже на месте (или не подходит): $(basename "$patch")"
    fi
done

# Одна архитектура за проход и одна сборка за раз (память машины — 32 ГБ,
# правило CLAUDE.md): никакого параллельного запуска обеих половин.
for arch in $ZARCHS; do
    prefix="$ZDEPS_BASE-$arch"
    bdir="$ZBUILD/qtbase-build-$arch"
    echo "=== qtbase [$arch] → $prefix ==="
    mkdir -p "$bdir"
    cd "$bdir"
    "$ZBUILD/qtbase/configure" \
        -prefix "$prefix" \
        -static -release \
        -opensource -confirm-license \
        -nomake examples -nomake tests \
        -qt-pcre -qt-harfbuzz -qt-doubleconversion -qt-libjpeg -qt-libpng -qt-zlib \
        -no-glib -no-icu -no-cups \
        -- \
        -DCMAKE_OSX_DEPLOYMENT_TARGET="$ZMACOS_MIN" \
        -DCMAKE_OSX_ARCHITECTURES="$arch"
    cmake --build . -j8
    cmake --install .

    echo "=== qtsvg [$arch] ==="
    sdir="$ZBUILD/qtsvg-build-$arch"
    mkdir -p "$sdir"
    cd "$sdir"
    # qt-cmake подставляет ключи из prefix — ни -static, ни пол, ни архитектуру
    # здесь повторять не надо, и разойтись им негде.
    "$prefix/bin/qt-cmake" "$ZBUILD/qtsvg" -DCMAKE_BUILD_TYPE=Release
    cmake --build . -j8
    cmake --install .
done

echo "=== Qt готов: $(for a in $ZARCHS; do printf '%s ' "$ZDEPS_BASE-$a"; done) ==="
