#!/bin/bash
#
# Переносимая сборка zametti под macOS от cmake до dmg: программа собирается
# отдельно под каждую архитектуру из $ZARCHS, половины склеиваются lipo в
# универсальный бинарь, тот заворачивается в Zametti.app с ad-hoc подписью,
# app — в dmg. Упаковка после склейки — дело секунд (слова владельца), поэтому
# она последний шаг ЭТОГО скрипта, а не отдельный; Zametti.app остаётся лежать
# рядом с dmg.
#
#   bash packaging/mac/build-qt.sh     # однажды, это часы
#   bash packaging/mac/build-app.sh    # каждая пересборка программы
#
# Итог — в build-portable-universal/: zametti (голый бинарь), Zametti.app,
# Zametti-<версия>.dmg. Внутри бинаря — статический Qt из $ZDEPS_BASE-<арх> и
# всё вендоренное добро (libjxl, libheif, libsodium, zstd, blake3, microtex…);
# снаружи остаются ТОЛЬКО системные рамки, которые есть на любом маке по
# построению и вкладывать которые нельзя.
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

# Облик приложения — всё в одном месте.
APP_NAME="Zametti"
APP_VERSION="0.9"
BUNDLE_ID="io.zametti.zametti"

# ── 1. Сборка каждой половины ────────────────────────────────────────────────
# Одна архитектура на configure и одна сборка за раз — почему, написано в
# zenv.sh (SIMD вендоренных библиотек) и в CLAUDE.md (память машины — 32 ГБ).
BINARIES=()
for arch in $ZARCHS; do
    prefix="$ZDEPS_BASE-$arch"
    [ -x "$prefix/bin/qt-cmake" ] || {
        echo "нет $prefix/bin/qt-cmake — сперва соберите Qt:" >&2
        echo "        ZARCHS=$arch bash packaging/mac/build-qt.sh" >&2
        exit 1
    }
    out="$ROOT/build-portable-$arch"
    echo "=== zametti [$arch] → $out ==="
    extra=()
    if [ "$arch" != "$ZHOSTARCH" ]; then
        # На чужой архитектуре cmake оставил бы в CMAKE_SYSTEM_PROCESSOR арх
        # машины, и libgav1/libsodium/highway/libjxl выбрали бы не тот SIMD
        # (см. zenv.sh). Задаём только процессор, БЕЗ CMAKE_SYSTEM_NAME: имя
        # включило бы полный кросс-режим cmake, которому Qt требует qt-host-path.
        extra+=("-DCMAKE_SYSTEM_PROCESSOR=$arch")
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
    BINARIES+=("$out/app/zametti")
done

# ── 2. Склейка и приёмка ломтей ──────────────────────────────────────────────
UNI="$ROOT/build-portable-universal"
mkdir -p "$UNI"
lipo -create "${BINARIES[@]}" -output "$UNI/zametti"

echo "=== приёмка: $UNI/zametti ==="
lipo -info "$UNI/zametti"
for arch in $ZARCHS; do
    minos="$(otool -arch "$arch" -l "$UNI/zametti" | awk '/LC_BUILD_VERSION/{v=1} v && /minos/{print $2; exit}')"
    if [ "$minos" != "$ZMACOS_MIN" ]; then
        echo "ломоть $arch: пол $minos вместо $ZMACOS_MIN" >&2
        exit 1
    fi
    echo "  ломоть $arch: пол $minos — верно"
done
# Чужих динамических библиотек снаружи быть не должно: только системные рамки.
foreign="$(otool -L "$UNI/zametti" | tail -n +2 | grep -v -E '/usr/lib/|/System/Library/' || true)"
if [ -n "$foreign" ]; then
    echo "снаружи остались чужие библиотеки:" >&2
    echo "$foreign" >&2
    exit 1
fi
echo "  чужих библиотек снаружи нет"
# Дымовой запуск ОБОИХ ломтей: x86_64 бежит под Rosetta. --help и --dump-config
# работают без дисплея (--dump-config сам поднимает offscreen QGuiApplication).
for arch in $ZARCHS; do
    echo "  дымовой запуск [$arch]:"
    arch "-$arch" "$UNI/zametti" --help > /dev/null
    arch "-$arch" "$UNI/zametti" --noconfig --dump-config > /dev/null
    echo "    --help и --noconfig --dump-config отработали"
done

# ── 3. Zametti.app ───────────────────────────────────────────────────────────
APP="$UNI/$APP_NAME.app"
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
cp "$UNI/zametti" "$APP/Contents/MacOS/zametti"

cat > "$APP/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleIdentifier</key>      <string>$BUNDLE_ID</string>
    <key>CFBundleName</key>            <string>$APP_NAME</string>
    <key>CFBundleExecutable</key>      <string>zametti</string>
    <key>CFBundleIconFile</key>        <string>zametti</string>
    <key>CFBundlePackageType</key>     <string>APPL</string>
    <key>CFBundleShortVersionString</key> <string>$APP_VERSION</string>
    <key>CFBundleVersion</key>         <string>$APP_VERSION</string>
    <key>LSMinimumSystemVersion</key>  <string>$ZMACOS_MIN</string>
    <key>NSHighResolutionCapable</key> <true/>
</dict>
</plist>
PLIST

# Иконка: .icns собирается на месте из resources/zametti1024x1024.png —
# бинарный icns в репозитории не живёт. 1024 — это и есть 512@2x.
ICONSET="$UNI/zametti.iconset"
rm -rf "$ICONSET"
mkdir -p "$ICONSET"
SRC_ICON="$ROOT/resources/zametti1024x1024.png"
for size in 16 32 128 256 512; do
    sips -z "$size" "$size" "$SRC_ICON" --out "$ICONSET/icon_${size}x${size}.png" > /dev/null
    double=$((size * 2))
    sips -z "$double" "$double" "$SRC_ICON" --out "$ICONSET/icon_${size}x${size}@2x.png" > /dev/null
done
iconutil -c icns "$ICONSET" -o "$APP/Contents/Resources/zametti.icns"
rm -rf "$ICONSET"

codesign --force --sign - "$APP"
codesign --verify --strict --deep "$APP"
echo "=== $APP_NAME.app подписан (ad-hoc) и проверен ==="

# ── 4. dmg ───────────────────────────────────────────────────────────────────
STAGE="$UNI/dmg-staging"
rm -rf "$STAGE"
mkdir -p "$STAGE"
cp -R "$APP" "$STAGE/"
ln -s /Applications "$STAGE/Applications"
DMG="$UNI/$APP_NAME-$APP_VERSION.dmg"
rm -f "$DMG"
hdiutil create -fmt UDZO -volname "$APP_NAME" -srcfolder "$STAGE" -ov "$DMG" > /dev/null
rm -rf "$STAGE"
hdiutil verify "$DMG" > /dev/null
echo "=== dmg собран и проверен ==="

echo
echo "=== что получилось ==="
ls -lh "$UNI/zametti" "$DMG" | awk '{print " ", $9, "—", $5}'
echo "  $APP — рядом, не удалён"
