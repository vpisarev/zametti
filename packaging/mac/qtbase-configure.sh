#!/bin/bash
#
# Настройка статического Qt (qtbase) под macOS.
#
#   git -C $ZBUILD clone --depth 1 --branch v6.10.3 https://code.qt.io/qt/qtbase.git
#   bash packaging/mac/qtbase-configure.sh
#   cd $ZBUILD/qtbase-build && cmake --build . -j8 && cmake --install .
#
# Записана файлом, чтобы её можно было повторить и прочитать, а не
# восстанавливать из памяти. НАСТРАИВАЕТ, но не собирает: сборка Qt долгая, и её
# код возврата надо видеть отдельно — `cmake --build . | tail` отдаёт $? от
# `tail`, и упавшая сборка отрапортует нулём (наступали на линуксе).
#
# Версия та же, что у переносимой сборки под линукс, — 6.10.3 (слово владельца):
# два разных Qt на двух системах означали бы два разных набора багов вёрстки, а
# сравнивать снимки приёмки надо с одним и тем же.
#
# Как читать список ключей:
#   -qt-      своя копия дешевле лишней динамической зависимости;
#   -no-      вещь тянет за собой мир (glib, ICU) или не нужна вовсе (CUPS — на
#             бумагу мы выводим своим PDF).
set -e

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=zenv.sh
source "$HERE/zenv.sh"

[ -d "$ZBUILD/qtbase" ] || { echo "нет дерева qtbase в $ZBUILD/qtbase" >&2; exit 1; }

# ЗАПЛАТЫ К QT. Каждая — с объяснением внутри самой заплаты; переносить их при
# смене версии Qt обязательно, и проверять, не починено ли наверху.
# Накладываются идемпотентно: --check перед --apply, чтобы повторный запуск
# скрипта не ломался на уже наложенной.
for patch in "$HERE"/patches/qtbase-*.patch; do
    [ -e "$patch" ] || continue
    if git -C "$ZBUILD/qtbase" apply --check "$patch" 2>/dev/null; then
        git -C "$ZBUILD/qtbase" apply "$patch"
        echo "заплата наложена: $(basename "$patch")"
    else
        echo "заплата уже на месте (или не подходит): $(basename "$patch")"
    fi
done

mkdir -p "$ZBUILD/qtbase-build"
cd "$ZBUILD/qtbase-build"

"$ZBUILD/qtbase/configure" \
    -prefix "$ZPREFIX" \
    -static -release \
    -opensource -confirm-license \
    -nomake examples -nomake tests \
    -qt-pcre -qt-harfbuzz -qt-doubleconversion -qt-libjpeg -qt-libpng -qt-zlib \
    -no-glib -no-icu -no-cups \
    -- \
    -DCMAKE_OSX_DEPLOYMENT_TARGET="$ZMACOS_MIN" \
    -DCMAKE_OSX_ARCHITECTURES="$ZARCHS"
