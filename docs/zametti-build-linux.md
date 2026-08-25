# Как собрать переносимый zametti под Linux

Пошаговая запись того, чем собран `build-portable/app/zametti` — один файл,
который запускается на Ubuntu 20.04 и всём, что новее, и не требует ни Qt, ни
OpenSSL, ни кодеков на целевой машине. Отчёт о том, ПОЧЕМУ каждое решение
такое, — `docs/zametti-portable-report.md` и `docs/zametti-av1-report.md`;
здесь только порядок действий и проверки после каждого шага.

Обычная сборка для себя ничего этого не требует: `cmake -S . -B build` берёт
системную Qt и живёт своей жизнью. Всё нижеследующее нужно ровно тогда, когда
программу надо ОТДАТЬ.

## 0. Что получается

| | |
|---|---|
| один файл | `build-portable/app/zametti`, 63 МБ |
| Qt | 6.10.3, статический, внутри |
| OpenSSL | 3.5.7 LTS, статический, внутри |
| картинки | jxl, jpeg, tiff, webp, avif, heic — всё вшито |
| динамических зависимостей | 30 |
| floor glibc | 2.31 — Ubuntu 20.04+, Debian 11+, RHEL 9+ |
| `GLIBCXX` | не требуется вовсе |
| заголовок окна под GNOME | родной, плагин `adwaita` |

Идея одна: **собирать не против системы, на которой сидим, а против sysroot
старой Ubuntu.** Компилятор при этом свежий (gcc 15), а заголовки и библиотеки
— из 20.04. Тогда планку задаёт sysroot, а не машина сборщика.

Три каталога, и путать их не надо:

| | | |
|---|---|---|
| `~/work/zsys` | sysroot | **перезаписывается тарболом целиком** |
| `~/work/zdeps` | всё, что мы собрали (OpenSSL, xcb, Qt) | тарбол его НЕ трогает — нарочно |
| `~/work/zbuild` | мусор сборки, исходники чужого | сносится безболезненно |

## 1. Что нужно на машине сборки

Хостовые: `cmake` (≥ 3.20), `ninja` или `make`, `perl`, `python3`, `m4`,
`curl`, `git`, `pkg-config`, `patch`. Они исполняются здесь и сейчас, поэтому
берутся с хоста нарочно — в toolchain так и записано
(`CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER`).

`m4` нужен не «на всякий случай»: два пакета из шести помощников xcb
(`xcb-util-wm`, `xcb-util-cursor`) генерируют им часть исходников, и без него
их `configure` падает на пятом шаге сообщением, из которого причина не видна.

**Память.** Сборка — `-j4`, по одной за раз. 16 компиляторов gcc по 1.5–2 ГБ
забивают 32 ГБ насмерть; это уже случалось (17.08.2026). Перед тяжёлым —
`free -g`.

## 2. Sysroot: `~/work/zsys`

### 2.1 Из чего он сделан

`usr/`, `lib/`, `lib64/` сняты тарболом с живой машины Ubuntu 20.04, на
которой доставлены dev-пакеты и gcc-15 из PPA (версия внутри —
`15.2.0-15ubuntu1~20~ppa2`, суффикс `~20~ppa2` и означает сборку под focal).
Тарбол лежит рядом: `~/work/zsys/zsys.tar`, 2.4 ГБ.

Базы пакетов (`var/lib/dpkg`) в дереве нет — это копия файлов, а не система.
Поэтому списка «что установлено» из него не достать; вместо него проверяемый
признак — какие `.pc` лежат внутри:

```
dbus-1 egl expat fontconfig freetype2 gl glx ice libcrypt libpng libpng16
libxcrypt pthread-stubs sm uuid wayland-client wayland-cursor wayland-egl
wayland-scanner wayland-server x11 x11-xcb xau xcb xcb-* xcursor xdmcp xext
xfixes xi xkbcommon xkbcommon-x11 xrandr xrender zlib
```

плюс в `usr/share/pkgconfig` — `wayland-protocols` и семейство `*proto`.
В переводе на пакеты focal это `libdbus-1-dev libegl-dev libgl-dev
libexpat1-dev libfontconfig1-dev libfreetype6-dev libice-dev libsm-dev
libxcrypt-dev libpng-dev uuid-dev libwayland-dev wayland-protocols
libx11-dev libx11-xcb-dev libxcb1-dev libxcb-*-dev libxcursor-dev libxext-dev
libxfixes-dev libxi-dev libxkbcommon-dev libxkbcommon-x11-dev libxrandr-dev
libxrender-dev zlib1g-dev`.

**Чего в sysroot нет НАРОЧНО:** glib, ICU, CUPS, OpenSSL, zstd, libb2, GTK,
Vulkan, libudev, libheif, libaom, libde265. Qt при настройке берёт всё, что
найдёт, и каждая найденная системная библиотека становится динамической
зависимостью результата. Пустота здесь — инструмент, а не недоделка.

### 2.2 Распаковка

```bash
mkdir -p ~/work/zsys && cd ~/work/zsys
tar -xf zsys.tar
```

### 2.3 Починка — ОБЯЗАТЕЛЬНО ПОСЛЕ КАЖДОЙ РАСПАКОВКИ

```bash
~/work/zsys/bin/fix-sysroot.sh
```

Дев-симлинки Ubuntu (`libpthread.so`, `libdl.so`, `librt.so`, `libuuid.so`,
`libcrypt.so`, `libdbus-1.so`, `libz.so` — 21 штука) указывают **абсолютным**
путём на `/lib/x86_64-linux-gnu/...`. Внутри sysroot такой путь разрешает не
линковщик от sysroot, а ядро от НАСТОЯЩЕГО корня: 15 из них вели на живой файл
хоста, и `-lpthread` молча брал бы glibc 2.43 вместо 2.31.

Это утечка переносимости без единого сообщения. Скрипт переписывает их в
относительные. **Архив приносит абсолютные заново — значит и запускать надо
заново.** Правильный вывод выглядит так:

```
sysroot: /home/vpisarev/work/zsys
  симлинков переписано в относительные: 21
  оставлено как есть (цели нет в sysroot): N
```

### 2.4 Обёртки компилятора

`~/work/zsys/bin/gcc` и `g++` — трёхстрочные обёртки:

```sh
#!/bin/sh
here=$(dirname "$(readlink -f "$0")")     # …/zsys/bin
root=$(dirname "$here")                   # …/zsys
export LD_LIBRARY_PATH="$root/hostlibs${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
exec "$root/usr/bin/gcc-15" --sysroot="$root" "$@"
```

`hostlibs/` — четыре библиотеки focal (`libisl`, `libbfd`, `libopcodes`),
без которых focal'ьный gcc-15 не запускается на хосте. Каталог сделан руками и
тарболом не затрагивается.

**Линкует при этом ХОСТОВЫЙ `ld`** (binutils хоста): `--sysroot` не переносит
`COMPILER_PATH`. Замерено, что `DT_RELR` он по умолчанию не выпускает, но
приёмка (§6) обязана это проверять, а не полагаться на умолчания.

### 2.5 `wayland-scanner` — 1.18, из sysroot

```
$ ~/work/zsys/usr/bin/wayland-scanner --version
wayland-scanner 1.18.0
```

Хостовый сканер (1.24) порождает код, зовущий `wl_proxy_marshal_flags` —
это wayland ≥ 1.20, а в sysroot заголовки 1.18, где такой функции нет. Соблазн
подложить свежий libwayland — ловушка: функция нужна не при сборке, а **на
целевой машине**, и на Ubuntu 20.04 программа не запустилась бы. Чинить надо в
сторону старого; ABI у wayland стабилен, старый API из новых libwayland никуда
не делся.

Сканер жалуется на хостовую libxml2 («no version information available») и
работает — это шум, а не отказ.

## 3. Окружение

Одной строкой перед КАЖДЫМ шагом:

```bash
source ~/work/zsys/bin/zenv.sh
```

Он задаёт `ZSYS`, `ZPREFIX`, `ZBUILD`, `CC`/`CXX` (обёртки), pkg-config сквозь
sysroot и общие флаги:

```sh
export PKG_CONFIG_SYSROOT_DIR="$ZSYS"
export PKG_CONFIG_LIBDIR="$ZSYS/usr/lib/x86_64-linux-gnu/pkgconfig:$ZSYS/usr/share/pkgconfig"
export ZCFLAGS="-O2 -fPIC"
export ZLDFLAGS="-static-libstdc++ -static-libgcc"
```

`PKG_CONFIG_LIBDIR`, а не `PKG_CONFIG_PATH`: первый **заменяет** список
каталогов, второй только дополняет, и системные `.pc` хоста остались бы
видны. `usr/share/pkgconfig` в списке обязателен — там `wayland-protocols.pc`.

`-fPIC` обязателен: всё собранное уедет внутрь PIE-программы.
`-static-libstdc++ -static-libgcc` **обязательны, а не желательны**: libstdc++
у gcc-15 требует `GLIBCXX_3.4.32`, а в самой Ubuntu 20.04 лежит 3.4.28 — без
них бинарь не запускается даже на той системе, с которой снят sysroot.

## 4. `~/work/zdeps`: OpenSSL, помощники xcb, Qt

Порядок: **OpenSSL → xcb → Qt**. Qt при настройке спрашивает OpenSSL, а
статические архивы xcb должны существовать раньше, чем их начнут искать.

### 4.1 OpenSSL 3.5.7 LTS

```bash
source ~/work/zsys/bin/zenv.sh
cd $ZBUILD && curl -fsSLO https://github.com/openssl/openssl/releases/download/openssl-3.5.7/openssl-3.5.7.tar.gz
tar -xf openssl-3.5.7.tar.gz && cd openssl-3.5.7

./Configure linux-x86_64 no-shared no-tests no-docs no-apps no-legacy \
    --prefix="$ZPREFIX" --openssldir=/etc/ssl -fPIC -O2 CC="$CC"
make -j4 && make install_sw
```

`--openssldir=/etc/ssl` — это путь на ЦЕЛЕВОЙ машине, где лежат её
доверенные корневые сертификаты. Записать сюда `$ZPREFIX` значило бы получить
программу, которая на чужой машине не доверяет никому.

`no-apps` убирает `openssl(1)`, который нам не нужен; `no-legacy` — старый
провайдер шифров.

### 4.2 Шесть помощников xcb

```bash
source ~/work/zsys/bin/zenv.sh
packaging/linux/build-xcb-static.sh
```

Скрипт скачивает шесть пакетов с freedesktop, собирает статикой с `-fPIC` в
`$ZPREFIX` и **сам проверяет результат** `readelf`'ом: ни одной релокации
`R_X86_64_32/32S`, иначе в PIE не вложить. Ожидаемый хвост вывода:

```
  libxcb-util.a: PIC
  libxcb-image.a: PIC
  ... (шесть строк)
```

Зачем вообще: эти шесть лежат в пакетах `priority=extra` — в базовую установку
они не входят никогда, и `libxcb-cursor0` из них даёт самую частую жалобу на
Qt 6.5+ вообще («could not load the Qt platform plugin xcb»). Вшив их, мы
снимаем шесть пакетов из требований к установке.

Почему из upstream, а не архивы из sysroot: Ubuntu собирает `libxcb-image.a` и
`libxcb-util.a` **без** `-fPIC`, а у focal'ьного `libxcb-cursor` в код вшит
устаревший список каталогов с темами курсоров — без `~/.local/share/icons`,
куда GNOME и KDE кладут пользовательские темы сегодня. Отсюда обязательный
`--with-cursorpath` в скрипте: по умолчанию путь собирается из `${datadir}`,
то есть из нашего `--prefix`, и программа искала бы системные курсоры в
каталоге сборки. Это нашлось только `strings` готового архива — сборка при
этом проходит молча и успешно.

### 4.3 Qt 6.10.3 — три модуля

```bash
source ~/work/zsys/bin/zenv.sh
cd $ZBUILD
for m in qtbase qtsvg qtwayland; do
    git clone --depth 1 --branch v6.10.3 https://code.qt.io/qt/$m.git
done
```

**qtbase** — настройка записана файлом `~/work/zbuild/qtbase-configure.sh`,
чтобы её можно было прочитать, а не восстанавливать из памяти:

```bash
"$ZBUILD/qtbase/configure" \
    -prefix "$ZPREFIX" \
    -static -release \
    -opensource -confirm-license \
    -nomake examples -nomake tests \
    -openssl-linked \
    -xcb -fontconfig \
    -system-freetype -system-zlib -system-libpng \
    -qt-pcre -qt-harfbuzz -qt-doubleconversion -qt-libjpeg \
    -no-glib -no-icu -no-cups \
    -- \
    -DCMAKE_TOOLCHAIN_FILE="$ZSYS/zsys-toolchain.cmake" \
    -DOPENSSL_ROOT_DIR="$ZPREFIX" \
    -DOPENSSL_USE_STATIC_LIBS=ON \
    -DWaylandScanner_EXECUTABLE="$ZSYS/usr/bin/wayland-scanner"
```

Читается так: **system-** там, где библиотека есть в sysroot и есть на любой
целевой машине (freetype, zlib, libpng); **qt-** там, где своя копия дешевле
лишней зависимости; **no-** там, где вещь тянет за собой мир (glib, ICU) или
не нужна вовсе (CUPS — печатаем мы своим PDF).

```bash
mkdir -p $ZBUILD/qtbase-build && cd $ZBUILD/qtbase-build
~/work/zbuild/qtbase-configure.sh
cmake --build . -j4 && cmake --install .
```

Сборка qtbase без QML/Quick/WebEngine — 822 цели, около трёх минут на восьми
ядрах. **Код возврата читать у сборки, а не у конвейера:** `cmake --build . |
tail` отдаёт `$?` от `tail`, и упавшая сборка отрапортует нулём (наступали).

**qtsvg** — нужен и нам (иконки Lucide), и плагину `adwaita`:

```bash
mkdir -p $ZBUILD/qtsvg-build && cd $ZBUILD/qtsvg-build
$ZPREFIX/bin/qt-cmake $ZBUILD/qtsvg -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=$ZPREFIX
cmake --build . -j4 && cmake --install .
```

**qtwayland** — ради одного плагина `adwaita` (родной заголовок окна под
GNOME) и с нашим патчем к нему:

```bash
source ~/work/zsys/bin/zenv.sh
packaging/linux/build-qtwayland.sh
```

Скрипт накладывает патчи из `packaging/linux/patches/` идемпотентно (уже
наложенный пропускается молча), настраивает, собирает, ставит и **проверяет
сводку**: без `Qt::DBus` или `Qt::Svg` условие фичи не выполняется, и плагин
молча не соберётся. Поэтому в скрипте стоит

```
grep -q "GNOME-like client-side decorations ... yes" config.summary || exit 1
```

Ожидаемый хвост:

```
наложен: qtwayland-adwaita-titlebar-font.patch
qtwayland: готово, adwaita в /home/vpisarev/work/zdeps/plugins/wayland-decoration-client/
```

Про сам патч: плагин берёт шрифт заголовка у платформенной темы, а
`QGnomeTheme` отдаёт для `TitleBarFont` `nullptr` — правильный шрифт умеет
только `QGtk3Theme`, а gtk3 мы не тянем нарочно (это +12 динамических
зависимостей, и в sysroot нет ни glib, ни gtk). Срабатывал запасной
`QFont("Cantarell", 10)`. Патч разбирает строку, которую плагин **и так уже
спрашивает у портала** (`org.gnome.desktop.wm.preferences/titlebar-font`,
например `Adwaita Sans Bold 11`) и из которой брал одно слово «bold», плюс
чинит имя ключа `titlebar-uses-system-font` и подписывает живое обновление.
Проверено, что в 6.11.2 и в dev обе беды на месте слово в слово — обновление
Qt не помогло бы.

Замечание из отчёта, которое здесь важно: **Qt 6.10 втянул wayland-КЛИЕНТ
внутрь qtbase**, и без модуля qtwayland программа под Wayland уже работает.
Модуль собирается ровно ради декорации.

## 5. Сама программа

```bash
source ~/work/zsys/bin/zenv.sh
cd ~/work/zametti
cmake -S . -B build-portable \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_TOOLCHAIN_FILE=packaging/linux/toolchains/linux-zsys.cmake \
      -DCMAKE_PREFIX_PATH=$ZPREFIX \
      -DWITH_STATIC_QT=ON
cmake --build build-portable -j4
```

`WITH_STATIC_QT=ON` ничего не переключает — он **стережёт**: если найденная Qt
окажется динамической, настройка падает с объяснением. Собрать переносимую
сборку против системной Qt и не заметить — слишком дёшево.

Что делает toolchain (`packaging/linux/toolchains/linux-zsys.cmake`) и почему —
в нём же, подробно. Три вещи важны здесь:

* `CMAKE_FIND_ROOT_PATH_MODE_{LIBRARY,INCLUDE,PACKAGE} ONLY` — без этого
  `find_library()` спокойно берёт `/usr/lib/x86_64-linux-gnu` **хоста**,
  сборка проходит, а переносимость утекает молча;
* шесть `XCB_*_LIBRARY` прибиваются к архивам из `$ZPREFIX`. Задавать их надо
  **здесь**, а не только при сборке Qt: `FindXCB.cmake` ставится вместе с Qt и
  переискивает библиотеки заново у каждого потребителя. Первая попытка задала
  их только у Qt, и из тридцати семи зависимостей ушла ровно одна;
* `CMAKE_SYSTEM_NAME` не задаётся нарочно — архитектура та же, сборка остаётся
  «родной», и Qt не требует `QT_HOST_PATH` со второй сборкой Qt на пустом
  месте.

**Плагины Qt перечисляются поимённо.** Автоматика `qt_add_executable` вписывает
одну платформу (xcb) и на этом останавливается — молча. Программа собирается,
окно показывает, и падает с SIGABRT на `--check` («Available platform plugins
are: xcb»); а на `offscreen` стоят и `--check`, и ВСЕ наборы. Заодно тихо
пропадают оба плагина ввода. Список — `zametti_import_qt_plugins()` в корневом
`CMakeLists.txt`, правило для него: **статическая сборка обязана вести себя
как динамическая**, то есть берём то, что динамический Qt подхватил бы сам, а
не «то, без чего не падает».

Сводка настройки печатает, что вошло:

```
* Qt:         `6.10.3 static`
* vendored libraries:
    * `libheif 1.23.2` — HEIF/AVIF container
    * `libde265 1.1.1` — HEVC decoder inside HEIC
    * `libgav1 0.20.0` — AV1 decoder inside AVIF
    ...
```

## 6. Приёмка

Ни один из этих шагов не «на всякий случай»: каждый ловил настоящую поломку.

**1. Зависимости — числом и поимённо.**

```bash
objdump -p build-portable/app/zametti | grep NEEDED | awk '{print $2}' | sort
```

Сегодня их 30. Список меняться должен только осознанно; **выросшее число —
это найденная системная библиотека**, то есть чаще всего недоделанный
вендоринг. Прошлый раз так ушла `libz.so.1`.

**2. Планка glibc и версионные теги.**

```bash
objdump -T build-portable/app/zametti | grep -o 'GLIBC_[0-9.]*' | sort -V -u | tail -1
readelf -d build-portable/app/zametti | grep -c RELR   # обязан быть 0
```

Ожидается `GLIBC_2.31` и ноль `DT_RELR`.

**3. Обе платформы и наборы.**

```bash
QT_QPA_PLATFORM=offscreen build-portable/app/zametti --check заметка.md
QT_QPA_PLATFORM=offscreen build-portable/tests/zametti-tests
```

Наборы гоняются в ОБЕИХ сборках — обычной и переносимой: разница между ними
уже ловила беды, которых по отдельности не видно (набор писал webp плагином
Qt, которого в статической сборке нет).

**4. Глазами — окно.** Заголовок под GNOME обязан быть гномовским, а не
серо-синим градиентом `bradient`. Механическая проверка, какая декорация
живая:

```bash
WAYLAND_DEBUG=1 build-portable/app/zametti 2>&1 | grep set_window_geometry | head -1
```

`(10, 10, ...)` — это `adwaita` с её тенями; `(0, 0, ...)` — запасной
`bradient`.

## 7. Что пока лежит вне репозитория

Честный список: четыре файла из рецепта в git не входят, и на новой машине их
придётся сделать руками.

| файл | что делает |
|---|---|
| `~/work/zsys/bin/zenv.sh` | окружение (§3) |
| `~/work/zsys/bin/fix-sysroot.sh` | починка симлинков (§2.3) |
| `~/work/zsys/bin/{gcc,g++}` | обёртки компилятора (§2.4) |
| `~/work/zsys/zsys-toolchain.cmake` | toolchain для сборки Qt и OpenSSL |
| `~/work/zbuild/qtbase-configure.sh` | настройка qtbase (§4.3) |

Их место — `packaging/linux/`, рядом с `build-xcb-static.sh` и
`build-qtwayland.sh`; тогда рецепт целиком лежал бы в репозитории и
проверялся бы вместе с ним. Пока это долг, названный вслух.

Второй долг оттуда же: `zsys-toolchain.cmake` (которым собрана Qt) и
`packaging/linux/toolchains/linux-zsys.cmake` (которым собрана программа) —
почти одинаковые, но не совсем: во втором есть привязка шести `XCB_*_LIBRARY`.
Два почти-одинаковых toolchain'а — это заготовка расхождения.

## 8. Дальше

Docker-образ со всем хозяйством просился бы сюда сам: sysroot + gcc-15 +
`~/work/zdeps` — это ровно то, что не меняется от сборки к сборке. Отложено
владельцем; когда дойдут руки, этот документ станет его Dockerfile'ом почти
построчно.

Рядом со временем лягут `packaging/win` и `packaging/mac`.
