# Как собрать переносимый zametti под Linux

Один файл, который запускается на Ubuntu 20.04 и всём, что новее, и не требует
на целевой машине ни Qt, ни OpenSSL, ни кодеков.

Обычная сборка для себя ничего этого не требует: `cmake -S . -B build` берёт
системную Qt и живёт своей жизнью. Всё нижеследующее нужно ровно тогда, когда
программу надо ОТДАТЬ.

**ПОЧЕМУ каждое решение такое** — в отчётах `docs/zametti-portable-report.md` и
`docs/zametti-av1-report.md`, а ловушки, на которых легко обжечься, записаны
прямо в скриптах и toolchain'ах `packaging/linux/`. Здесь — только порядок
действий и проверки.

| | |
|---|---|
| один файл | `build-portable/app/zametti`, 63 МиБ |
| Qt | 6.10.3, статический, внутри |
| OpenSSL | 3.5.7 LTS, статический, внутри |
| картинки | jxl, jpeg, tiff, webp, avif, heic — всё вшито |
| динамических зависимостей | 30 |
| планка glibc | 2.31 — Ubuntu 20.04+, Debian 11+, RHEL 9+ |
| `GLIBCXX` | не требуется вовсе |
| заголовок окна под GNOME | родной, плагин `adwaita` |

Идея одна: **собирать не против системы, на которой сидим, а против sysroot
старой Ubuntu.** Компилятор свежий (gcc 15), заголовки и библиотеки — из 20.04.
Тогда планку задаёт sysroot, а не машина сборщика.

## 1. Сборка

Когда подготовка (§2) сделана — всё:

```bash
cmake -S . -B build-portable -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_TOOLCHAIN_FILE=packaging/linux/toolchains/linux-zsys.cmake \
      -DWITH_STATIC_QT=ON
cmake --build build-portable -j8
```

Ни `source`, ни переменных окружения: toolchain сам знает, где sysroot и
собранные зависимости (`~/work/zsys` и `~/work/zdeps`), и запоминает это в кэше
сборки — `make` зовёт cmake заново на всякой правке `CMakeLists.txt`, и
окружения у него уже нет. Свои пути — `-DZSYS_ROOT=…`, `-DZDEPS_ROOT=…`.

`WITH_STATIC_QT=ON` ничего не переключает, он **стережёт**: если найденная Qt
окажется динамической, настройка падает с объяснением. Собрать переносимую
сборку против системной Qt и не заметить — слишком дёшево.

**Память:** только `-j8` и по одной сборке за раз. Шестнадцать gcc по 1.5–2 ГБ
забивают 32 ГБ насмерть (случалось 17.08.2026).

## 2. Подготовка — один раз

Три каталога, и путать их не надо:

| | | |
|---|---|---|
| `~/work/zsys` | sysroot Ubuntu 20.04 | **перезаписывается тарболом целиком** |
| `~/work/zdeps` | что мы собрали: OpenSSL, xcb, Qt | тарбол его НЕ трогает — нарочно |
| `~/work/zbuild` | мусор сборки и чужие исходники | сносится безболезненно |

На машине сборки нужны хостовые `cmake` (≥ 3.20), `make` или `ninja`, `perl`,
`python3`, `m4`, `curl`, `git`, `pkg-config`, `patch`. Они исполняются здесь и
сейчас, поэтому берутся с хоста нарочно.

### 2.1 Sysroot

```bash
mkdir -p ~/work/zsys && cd ~/work/zsys && tar -xf zsys.tar
bash packaging/linux/fix-sysroot.sh      # ОБЯЗАТЕЛЬНО ПОСЛЕ КАЖДОЙ РАСПАКОВКИ
```

Тарбол снят с живой Ubuntu 20.04 с dev-пакетами и gcc-15 из PPA. Починка
переписывает 21 дев-симлинк из абсолютных в относительные (иначе `-lpthread`
молча берёт glibc хоста) и ставит в `$ZSYS/bin` обёртки компилятора. Должна
напечатать:

```
sysroot: /home/vpisarev/work/zsys
  симлинков переписано в относительные: 21
```

Проверить, что сканер wayland — из sysroot, а не с хоста:

```bash
~/work/zsys/usr/bin/wayland-scanner --version    # ждём 1.18.0
```

### 2.2 OpenSSL → xcb → Qt

Порядок именно такой: Qt при настройке спрашивает OpenSSL, а статические архивы
xcb должны существовать раньше, чем их начнут искать.

```bash
source packaging/linux/zenv.sh    # нужен ТОЛЬКО здесь, для сборки зависимостей

cd $ZBUILD
curl -fsSLO https://github.com/openssl/openssl/releases/download/openssl-3.5.7/openssl-3.5.7.tar.gz
tar -xf openssl-3.5.7.tar.gz && cd openssl-3.5.7
./Configure linux-x86_64 no-shared no-tests no-docs no-apps no-legacy \
    --prefix="$ZPREFIX" --openssldir=/etc/ssl -fPIC -O2 CC="$CC"
make -j8 && make install_sw
```

`--openssldir=/etc/ssl` — путь на ЦЕЛЕВОЙ машине, где лежат её корневые
сертификаты; наш `$ZPREFIX` тут означал бы программу, не доверяющую никому.

```bash
packaging/linux/build-xcb-static.sh          # шесть помощников xcb, окружение берёт само
```

Скрипт сам проверяет результат `readelf`'ом: ни одной релокации `R_X86_64_32/32S`,
иначе в PIE-программу их не вложить. Ждём шесть строк вида `libxcb-util.a: PIC`.

```bash
cd $ZBUILD
for m in qtbase qtsvg qtwayland; do
    git clone --depth 1 --branch v6.10.3 https://code.qt.io/qt/$m.git
done

bash packaging/linux/qtbase-configure.sh                      # только настраивает
cd $ZBUILD/qtbase-build && cmake --build . -j8 && cmake --install .

mkdir -p $ZBUILD/qtsvg-build && cd $ZBUILD/qtsvg-build
$ZPREFIX/bin/qt-cmake $ZBUILD/qtsvg -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=$ZPREFIX
cmake --build . -j8 && cmake --install .

packaging/linux/build-qtwayland.sh           # ради одного плагина adwaita, с нашим патчем
```

Ключи Qt и причина каждого — в самом `qtbase-configure.sh`. **Код возврата
читать у сборки, а не у конвейера:** `cmake --build . | tail` отдаёт `$?` от
`tail`, и упавшая сборка отрапортует нулём (наступали).

`qtsvg` нужен и нам (иконки Lucide), и плагину `adwaita`. `qtwayland` собирается
ровно ради этого плагина: сам wayland-клиент в Qt 6.10 уже внутри qtbase.

## 3. Приёмка

Ни один шаг не «на всякий случай» — каждый ловил настоящую поломку.

```bash
B=build-portable/app/zametti
objdump -p $B | grep NEEDED | awk '{print $2}' | sort | wc -l   # 30
objdump -T $B | grep -o 'GLIBC_[0-9.]*' | sort -V -u | tail -1  # GLIBC_2.31
objdump -T $B | grep -c GLIBCXX                                  # 0
readelf -d $B | grep -c RELR                                     # 0
QT_QPA_PLATFORM=offscreen build-portable/tests/zametti-tests
```

**Выросшее число зависимостей — это найденная системная библиотека**, то есть
чаще всего недоделанный вендоринг: так однажды ушла `libz.so.1`. Наборы гоняются
в ОБЕИХ сборках, обычной и переносимой: разница между ними уже ловила беды,
которых по отдельности не видно (набор писал webp плагином Qt, которого в
статической сборке нет).

**Глазами — окно.** Заголовок под GNOME обязан быть гномовским, а не серо-синим
градиентом `bradient`. Механически:

```bash
WAYLAND_DEBUG=1 build-portable/app/zametti 2>&1 | grep set_window_geometry | head -1
```

`(10, 10, …)` — это `adwaita` с её тенями; `(0, 0, …)` — запасной `bradient`.

## 4. Что осталось снаружи и что должно

В репозитории лежит весь рецепт: `zenv.sh`, `fix-sysroot.sh`,
`qtbase-configure.sh`, оба toolchain'а, обёртки компилятора и скрипты сборки
зависимостей. Снаружи — ровно то, что в git положить нельзя:

| | |
|---|---|
| `~/work/zsys/zsys.tar` | сам sysroot, 2.4 ГБ двоичного |
| `~/work/zsys/hostlibs/` | четыре библиотеки focal, без которых focal'ьный gcc-15 не запускается на хосте |

**Долг, названный вслух:** `toolchains/qt-zsys.cmake` (им собрана Qt) и
`toolchains/linux-zsys.cmake` (им собирается программа) почти одинаковы —
разница ровно в том, что во втором прибиты шесть `XCB_*_LIBRARY` к статическим
архивам. Свести их в один можно, но это меняет то, чем собрана уже стоящая Qt,
а значит требует её пересборки и проверки; отложено осознанно.

Docker-образ со всем хозяйством просится сюда сам: sysroot + gcc-15 + `~/work/zdeps`
не меняются от сборки к сборке. Отложено владельцем — когда дойдут руки, этот
документ станет Dockerfile'ом почти построчно.

Кросс-сборка под Windows устроена по образцу этого рецепта и описана в
`docs/zametti-build-windows.md`; sysroot ей не нужен — целевой мир mingw с
хостовым не пересекается вовсе.
