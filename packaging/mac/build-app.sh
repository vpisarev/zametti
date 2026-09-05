#!/bin/bash
#
# Переносимая сборка zametti под macOS от cmake до dmg — ПО ОДНОМУ dmg НА
# АРХИТЕКТУРУ (решение владельца, 04.09.2026): intel-маков осталось немного, а
# универсальный бинарь носил бы каждому пользователю 30 МБ чужого кода.
#
#   bash packaging/mac/build-qt.sh          # однажды, это часы
#   bash packaging/mac/build-app.sh [arm|intel|all]   # программа; умолчание arm
#
# Что получается (раскладка build-portable/ — слово владельца: сборки по
# подпапкам архитектур, результат — в корне):
#
#   build-portable/<арх>/            cmake-дерево половины
#   build-portable/<арх>/Zametti.app программа этой архитектуры, подпись ad-hoc
#   build-portable/Zametti-<версия>-arm.dmg
#   build-portable/Zametti-<версия>-intel.dmg
#
# Версия — из project(... VERSION) корневого CMakeLists.txt: cmake при
# настройке пишет её в generated/version.txt и порождает generated/Info.plist
# из packaging/mac/Info.plist.in. Второго числа версии в скрипте нет.
#
# Внутри бинаря — статический Qt из $ZDEPS/<арх> и всё вендоренное добро
# (libjxl, libheif, libsodium, zstd, blake3, microtex…); снаружи остаются
# ТОЛЬКО системные рамки, которые есть на любом маке по построению и
# вкладывать которые нельзя. Символы стрипаются (решение владельца,
# 04.09.2026): таблица символов у arm64-ломтя весила ~7 МБ из 35.
#
# Подпись — ad-hoc (codesign -s -): без неё арм-маки бинарь вообще не запустят.
# Предупреждение Gatekeeper на ЧУЖОМ маке она не снимает — это снимают только
# Developer ID и нотаризация, отдельная работа (docs/zametti-build-macos.md,
# «Долги»).
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
# shellcheck source=zenv.sh
source "$HERE/zenv.sh"

APP_NAME="Zametti"

usage() {
    echo "usage: bash packaging/mac/build-app.sh [arm|intel|all]   (default: arm)" >&2
    exit 2
}
[ $# -le 1 ] || usage
case "${1:-arm}" in
    arm)   ARCHS="arm64" ;;
    intel) ARCHS="x86_64" ;;
    all)   ARCHS="arm64 x86_64" ;;
    *)     usage ;;
esac
# Имя половины в имени dmg — человеческое, а не машинное: «arm» и «intel»
# понятны тому, кто выбирает файл для скачивания; «arm64» и «x86_64» — нет.
tag_of() {
    case "$1" in
        arm64)  echo arm ;;
        x86_64) echo intel ;;
    esac
}

UNI="$ROOT/build-portable"
mkdir -p "$UNI"
APP_VERSION=""

# ── Иконка: .icns собирается на месте из resources/zametti_alt_1024x1024.png ──
# Один раз на запуск, обеим половинам одна и та же. Бинарный icns в
# репозитории не живёт. 1024 — это и есть 512@2x.
ICNS="$UNI/zametti.icns"
make_icns() {
    local iconset="$UNI/zametti.iconset"
    local src="$ROOT/resources/zametti_alt_1024x1024.png"
    rm -rf "$iconset"
    mkdir -p "$iconset"
    local size double
    for size in 16 32 128 256 512; do
        sips -z "$size" "$size" "$src" --out "$iconset/icon_${size}x${size}.png" > /dev/null
        double=$((size * 2))
        sips -z "$double" "$double" "$src" --out "$iconset/icon_${size}x${size}@2x.png" > /dev/null
    done
    iconutil -c icns "$iconset" -o "$ICNS"
    rm -rf "$iconset"
}

# ── 1. Сборка одной половины ─────────────────────────────────────────────────
# Одна архитектура на configure и одна сборка за раз — почему, написано в
# zenv.sh (SIMD вендоренных библиотек) и в CLAUDE.md (память машины — 32 ГБ).
build_arch() {
    local arch="$1"
    local prefix="$ZDEPS/$arch"
    local out="$UNI/$arch"
    [ -x "$prefix/bin/qt-cmake" ] || {
        echo "нет $prefix/bin/qt-cmake — сперва соберите Qt:" >&2
        echo "        ZARCHS=$arch bash packaging/mac/build-qt.sh" >&2
        exit 1
    }
    echo "=== zametti [$arch] → $out ==="
    local extra=()
    if [ "$arch" != "$ZHOSTARCH" ]; then
        # На чужой архитектуре cmake оставил бы в CMAKE_SYSTEM_PROCESSOR арх
        # машины, и libgav1/libsodium/highway/libjxl выбрали бы не тот SIMD
        # (см. zenv.sh). Задаём только процессор, БЕЗ CMAKE_SYSTEM_NAME: имя
        # включило бы полный кросс-режим cmake со своими требованиями.
        extra+=("-DCMAKE_SYSTEM_PROCESSOR=$arch")
        # Кросс-собранный Qt записал в свой toolchain-файл требование
        # QT_HOST_PATH — родного Qt с инструментами сборки (см. build-qt.sh).
        extra+=("-DQT_HOST_PATH=$ZDEPS/$ZHOSTARCH")
    fi
    # ${extra[@]+...} вместо голого "${extra[@]}": системный bash на маке —
    # 3.2, где пустой массив под set -u считается необъявленной переменной.
    "$prefix/bin/qt-cmake" -S "$ROOT" -B "$out" \
        -DCMAKE_BUILD_TYPE=Release \
        -DWITH_STATIC_QT=ON \
        -DCMAKE_OSX_DEPLOYMENT_TARGET="$ZMACOS_MIN" \
        -DCMAKE_OSX_ARCHITECTURES="$arch" \
        ${extra[@]+"${extra[@]}"}
    cmake --build "$out" -j8 --target zametti

    # Версия — от cmake, одна на обе половины.
    local version
    version="$(cat "$out/generated/version.txt")"
    [ -n "$version" ] || { echo "cmake не записал версию в $out/generated/version.txt" >&2; exit 1; }
    if [ -n "$APP_VERSION" ] && [ "$APP_VERSION" != "$version" ]; then
        echo "половины разошлись версиями: $APP_VERSION и $version" >&2
        exit 1
    fi
    APP_VERSION="$version"

    # ── Стрип и приёмка ломтя ────────────────────────────────────────────────
    local bin="$out/app/zametti"
    local before after
    before="$(stat -f %z "$bin")"
    strip "$bin"
    after="$(stat -f %z "$bin")"
    echo "  стрип: $((before / 1048576)) МБ → $((after / 1048576)) МБ"

    echo "=== приёмка [$arch]: $bin ==="
    lipo -info "$bin"
    local minos
    minos="$(otool -l "$bin" | awk '/LC_BUILD_VERSION/{v=1} v && /minos/{print $2; exit}')"
    if [ "$minos" != "$ZMACOS_MIN" ]; then
        echo "ломоть $arch: пол $minos вместо $ZMACOS_MIN" >&2
        exit 1
    fi
    echo "  пол $minos — верно"
    # Чужих динамических библиотек снаружи быть не должно: только системные
    # рамки. Заголовок «путь:» стоит в колонке 0, строки библиотек начинаются
    # с табуляции; отбираем только вторые.
    local foreign
    foreign="$(otool -L "$bin" | grep $'^\t' | grep -v -E '/usr/lib/|/System/Library/' || true)"
    if [ -n "$foreign" ]; then
        echo "снаружи остались чужие библиотеки:" >&2
        echo "$foreign" >&2
        exit 1
    fi
    echo "  чужих библиотек снаружи нет"
    # Дымовой запуск: x86_64 бежит под Rosetta. --help, --version и
    # --dump-config работают без дисплея (--dump-config сам поднимает
    # offscreen QGuiApplication). Стрипованный бинарь обязан запускаться —
    # это и проверяем.
    echo "  дымовой запуск [$arch]:"
    arch "-$arch" "$bin" --help > /dev/null
    local said
    said="$(arch "-$arch" "$bin" --version)"
    if [ "$said" != "zametti $APP_VERSION" ]; then
        echo "--version сказал «$said», а cmake — «$APP_VERSION»" >&2
        exit 1
    fi
    arch "-$arch" "$bin" --noconfig --dump-config > /dev/null
    echo "    --help, --version ($said) и --noconfig --dump-config отработали"
}

# ── 2. Zametti.app и dmg одной половины ──────────────────────────────────────
package_arch() {
    local arch="$1"
    local out="$UNI/$arch"
    local app="$out/$APP_NAME.app"
    rm -rf "$app"
    mkdir -p "$app/Contents/MacOS" "$app/Contents/Resources"
    cp "$out/app/zametti" "$app/Contents/MacOS/zametti"
    cp "$out/generated/Info.plist" "$app/Contents/Info.plist"
    cp "$ICNS" "$app/Contents/Resources/zametti.icns"

    codesign --force --sign - "$app"
    codesign --verify --strict --deep "$app"
    echo "=== $app подписан (ad-hoc) и проверен ==="

    local stage="$out/dmg-staging"
    rm -rf "$stage"
    mkdir -p "$stage"
    cp -R "$app" "$stage/"
    ln -s /Applications "$stage/Applications"
    local dmg="$UNI/$APP_NAME-$APP_VERSION-$(tag_of "$arch").dmg"
    rm -f "$dmg" "$out/raw.dmg"
    # НЕ hdiutil create -srcfolder: тот по дороге МОНТИРУЕТ образ в /Volumes, а
    # монтирование может быть запрещено (песочница агентов, CI). makehybrid
    # строит файловую систему напрямую, convert сжимает в UDZO — оба без
    # монтирования.
    hdiutil makehybrid -hfs -hfs-volume-name "$APP_NAME" -o "$out/raw.dmg" "$stage" > /dev/null
    hdiutil convert "$out/raw.dmg" -format UDZO -o "$dmg" > /dev/null
    rm -f "$out/raw.dmg"
    rm -rf "$stage"
    hdiutil verify "$dmg" > /dev/null
    echo "=== $dmg собран и проверен ==="
    RESULTS+=("$dmg")
}

RESULTS=()
make_icns
for arch in $ARCHS; do
    build_arch "$arch"
    package_arch "$arch"
done
rm -f "$ICNS"

echo
echo "=== что получилось (версия $APP_VERSION) ==="
for dmg in "${RESULTS[@]}"; do
    ls -lh "$dmg" | awk '{print " ", $9, "—", $5}'
done
for arch in $ARCHS; do
    echo "  $UNI/$arch/$APP_NAME.app — рядом, не удалён"
done
