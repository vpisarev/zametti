# Как собрать zametti под Windows, не выходя из Linux

Пошаговая запись того, чем собран `build-win/app/zametti.exe` — один файл,
который не требует на целевой машине ни Qt, ни OpenSSL, ни кодеков, ни
рантайма mingw. Собирается он **кросс-компиляцией с Linux** через mingw-w64;
отдельная машина с Windows для сборки не нужна вовсе, нужна только для приёмки.

Почему кросс, а не MSVC на самой Windows: всё чужое, кроме Qt, у нас уже
завендорено и собирается своим CMake, а Qt под mingw настраивается тем же
скриптом, что и под Linux. Вторая машина в круге сборки — это вторая машина,
на которой надо повторять весь рецепт.

Отчёт о том, ПОЧЕМУ каждое решение такое, — `docs/zametti-windows-report.md`;
здесь только порядок действий и проверки после каждого шага. Рядом лежит
`docs/zametti-build-linux.md` — переносимая сборка под Linux, устроенная по
тому же образцу.

## 0. Что получается

| | |
|---|---|
| один файл | `build-win/app/zametti.exe` |
| Qt | 6.10.3, статическая, внутри |
| TLS | schannel — свой у Windows, OpenSSL не нужен вовсе |
| картинки | jxl, jpeg, tiff, webp, avif, heic — всё вшито |
| рантайм mingw | вшит (`-static`): ни `libstdc++-6.dll`, ни `libgcc_s_seh-1.dll`, ни `libwinpthread-1.dll` рядом |
| keyring | Credential Manager (Advapi32) |
| подсистема | GUI, с подцепкой к консоли родителя ради `--check` |

Три каталога, и путать их не надо:

| | | |
|---|---|---|
| `/usr/x86_64-w64-mingw32` | целевой мир (пакет `mingw-w64`) | ставится системой |
| `~/work/zwin` | всё, что мы собрали под Windows (Qt) | наше |
| `~/work/zbuild` | мусор сборки, исходники чужого | общий с Linux-рецептом, сносится безболезненно |

## 1. Что нужно на машине сборки

```bash
sudo apt install mingw-w64 cmake ninja-build wine
```

`wine` не обязателен для сборки, но обязателен для проверки: без него `.exe`
не запустить, и `ctest` в каталоге сборки скажет об этом вслух (toolchain
подставляет wine в `CMAKE_CROSSCOMPILING_EMULATOR`, а не найдя — печатает
предупреждение).

**Хостовая Qt той же версии.** Кросс-сборка Qt берёт `moc`, `rcc`, `uic` и
`syncqt` у хостовой сборки — они исполняются здесь и сейчас. Годится та, что
собрана Linux-рецептом в `~/work/zdeps` (§4.3 в `zametti-build-linux.md`):
статичность инструментам не мешает, важна только версия. Проверить:

```bash
~/work/zdeps/libexec/moc --version     # ожидается moc 6.10.3
```

**Память.** Сборка — `-j8`, по одной за раз. Правило то же, что у Linux-рецепта,
и по той же причине (инцидент 17.08.2026). Перед тяжёлым — `free -g`.

## 2. Окружение

Одной строкой перед КАЖДЫМ шагом:

```bash
source packaging/win/zenv.sh
```

Он задаёт `ZWTRIPLE`, `ZWPREFIX`, `ZWBUILD`, `QT_HOST_PATH`, `CC`/`CXX` и
pkg-config под цель, а также громко жалуется, если mingw не поставлен или в
`QT_HOST_PATH` нет `libexec/moc`. Пути можно задать снаружи:
`ZWPREFIX=/иное/место source packaging/win/zenv.sh`.

Toolchain — `packaging/win/toolchains/mingw-w64.cmake`, и всё существенное
объяснено в нём же. Здесь важны четыре вещи:

* **драйверы с суффиксом `-posix`**, а не голые. У mingw две модели нитей, и
  голый `x86_64-w64-mingw32-g++` — это «win32», в которой pthread нет вовсе. В
  «posix» pthread есть (winpthreads, вшивается `-static`). Выбрана posix ради
  ЧУЖИХ деревьев: libgav1 закрывает свою Windows-ветку `#if defined(_MSC_VER)`,
  и mingw уходит в `pthread_create`. Подробности и почему не патч — в отчёте;
* `CMAKE_FIND_ROOT_PATH_MODE_{LIBRARY,INCLUDE,PACKAGE} ONLY` — иначе
  `find_library()` берёт хостовые `.so`, и беда всплывает только на компоновке;
* `-static -static-libgcc -static-libstdc++` — иначе рядом с программой лягут
  три DLL, и «у меня работает» будет означать лишь, что они нашлись в каталоге
  сборки;
* `-D_WIN32_WINNT=0x0A00 -DNOMINMAX` через `CMAKE_*_FLAGS_INIT`, а не
  `add_compile_definitions`: toolchain читается и внутри `try_compile`, и
  проверки чужих CMake обязаны видеть тот же набор, что и сама сборка.

## 3. Qt 6.10.3 под Windows

Исходники — те же, что у Linux-рецепта; если их ещё нет:

```bash
source packaging/win/zenv.sh
cd $ZWBUILD
for m in qtbase qtsvg; do
    git clone --depth 1 --branch v6.10.3 https://code.qt.io/qt/$m.git
done
```

**qtbase** — настройка записана файлом, чтобы её можно было прочитать, а не
восстанавливать из памяти:

```bash
bash packaging/win/qtbase-configure.sh          # только настраивает
cd $ZWBUILD/qtbase-win
cmake --build . -j8 && cmake --install .
```

Читается список так же, как под Linux, только «system-» здесь нет ни одного:
на целевой машине нашего добра нет, а Windows своих zlib/png/freetype не даёт.
Отсюда `-qt-zlib -qt-libpng -qt-freetype -qt-harfbuzz -qt-pcre
-qt-doubleconversion -qt-libjpeg`, `-no-icu -no-dbus -no-glib`,
`-opengl desktop`.

**`-schannel` вместо OpenSSL** — и это не экономия, а то же решение, что и под
Linux, только другим средством: доверенные корни должны быть у ЦЕЛЕВОЙ машины.
Под Linux ради этого OpenSSL линкуется с `--openssldir=/etc/ssl`; у Windows
хранилище корней своё, живое и обновляемое системой, и лезть со своим было бы
хуже.

**Код возврата читать у сборки, а не у конвейера:** `cmake --build . | tail`
отдаёт `$?` от `tail`, и упавшая сборка отрапортует нулём.

Ожидаемое в `config.summary`: `Schannel .... yes`, `DirectWrite .... yes`,
`Styles ... Fusion Windows WindowsVista`, `OpenSSL .... no`.

**qtsvg** — иконки тулбара, модуль (а не плагин `imageformats`):

```bash
source packaging/win/zenv.sh
mkdir -p $ZWBUILD/qtsvg-win && cd $ZWBUILD/qtsvg-win
$ZWPREFIX/bin/qt-cmake $ZWBUILD/qtsvg -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=$ZWPREFIX
cmake --build . -j8 && cmake --install .
```

`qtwayland` под Windows не нужен вовсе: заголовок окна рисует система.

## 4. Сама программа

```bash
source packaging/win/zenv.sh
cd ~/work/zametti
cmake -S . -B build-win \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_TOOLCHAIN_FILE=packaging/win/toolchains/mingw-w64.cmake \
      -DCMAKE_PREFIX_PATH=$ZWPREFIX \
      -DWITH_STATIC_QT=ON
cmake --build build-win -j8
```

`WITH_STATIC_QT=ON` ничего не переключает — он **стережёт**: найденная
динамическая Qt валит настройку с объяснением.

**Плагины Qt перечисляются поимённо**, и список зависит от системы —
`zametti_import_qt_plugins()` в корневом `CMakeLists.txt`. Под Windows это
`windows` (платформа), `offscreen` (режим `--check` и ВСЕ наборы), `minimal` и
`modernwindows` (облик окна: с Qt 6.7 стили — плагины, и без него
`QStyleFactory` отдаёт только Fusion). Правило то же, что под Linux:
статическая сборка обязана вести себя как динамическая.

## 5. Приёмка

Ни один из этих шагов не «на всякий случай».

**1. Зависимости — поимённо.**

```bash
objdump -p build-win/app/zametti.exe | grep 'DLL Name' | sort -u
```

Ожидаются ТОЛЬКО системные библиотеки Windows. Появление
`libstdc++-6.dll`, `libgcc_s_seh-1.dll` или `libwinpthread-1.dll` означает,
что `-static` где-то потерялся; появление чего-то ещё — найденную чужую
библиотеку, то есть недоделанный вендоринг.

**2. Командный режим и то, что он печатает.**

```bash
wine build-win/app/zametti.exe --check tests/канон-без-дисплея.md ; echo $?
```

Ноль и видимый вывод. Программа собрана подсистемой GUI (чтобы запуск из
проводника не открывал чёрное окно), и вывод виден только потому, что `main()`
первым делом подцепляется к консоли родителя.

**3. Наборы.**

```bash
cd build-win && ctest --output-on-failure
```

Toolchain подставил wine в `CMAKE_CROSSCOMPILING_EMULATOR`, поэтому `.exe`
запускаются как родные. Это ДЫМОВАЯ проверка: «зелено под wine» и «зелено под
Windows» — разные утверждения.

**4. Keyring.**

```bash
wine build-win/tests/zametti-bench.exe keyring
```

Пишет ключ и пароль сервера в Credential Manager, читает обратно, удаляет.

**5. Глазами — окно.**

```bash
wine build-win/app/zametti.exe
```

**6. Наборы под Linux — тоже.** Windows-заход трогает общий код (единая
`writeNewFile`, единый `configDir`), и Linux от этого пострадать не должен.
Debug и Release.

## 6. Что осталось за скобками

Установщик, подпись кода, ассоциация с `.md` и иконка внутри `.exe` — отдельная
задача упаковки. Приёмка на живой машине с Windows — за владельцем: wine ловит
непереносимый путь, текстовый режим файла и отсутствующий плагин платформы, но
про облик окна, DPI и родные диалоги отвечает не он.
