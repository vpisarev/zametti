# Верхняя граница «...4.0» — это не требование иметь CMake 4, а обещание, что с
# ним всё в порядке: политики применяются вплоть до 4.0, и предупреждений о
# старых умолчаниях он больше не печатает. Нижняя остаётся 3.20 нарочно —
# голое `VERSION 4.0` потребовало бы четвёртую от всех, а в Ubuntu 24.04 LTS до
# сих пор 3.28.
cmake_minimum_required(VERSION 3.20...4.0)
project(zametti LANGUAGES C CXX)

# Чужие деревья объявляют свои минимумы сами (в libtiff это 3.9), и CMake 4
# ворчит на каждое. Наша сборка от этого не зависит, а вывод настройки тонет:
# именно в нём владелец не смог разглядеть настоящую ошибку, собирая на маке.
# Гасим ворчание ДО add_subdirectory — на наш собственный код оно и не
# распространялось.
set(CMAKE_WARN_DEPRECATED OFF CACHE BOOL "suppress warnings about very old CMake versions being asked by 3rdparty libs")

# По умолчанию — Release. Отладочная сборка нужна нам и будущим участникам, а
# всем прочим достаётся то, что они и ждут: оптимизированная программа.
# Умолчание CMake (пустой тип) даёт сборку вовсе без -O, и владелец дважды
# собрал такую по случайности, а потом удивлялся, что программа подтормаживает.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
                 Debug Release RelWithDebInfo MinSizeRel)
    message(NOTICE "CMAKE_BUILD_TYPE is not specified — let's build a Release")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 99)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(ZAMETTI_WARNINGS_AS_ERRORS "warnings are errors" OFF)

# --- что тащить за собой ---------------------------------------------------
#
# Уговор: у готовой программы чужих библиотек рядом быть не должно. Наше
# вендоренное добро линкуется внутрь, системного мы просим как можно меньше, а
# остаётся то, без чего на этой машине жить нельзя вовсе — сам Qt (если он
# динамический), libc и системные рамки.

# Статический Qt НЕ включается ключом — он либо есть на машине, либо нет:
# динамический Qt статическим не станет ни от каких флагов. Ключ означает
# другое: «я собираю ради самодостаточной программы, и молча получить
# динамический Qt не согласен». Без него сборка одинаково рада обоим.
#
# Именно здесь владелец обжёгся: совет из сети предлагал добавить -static в
# флаги компоновки, и статическими стали заодно системная libheif с кодеками,
# которых в таком виде на машине не оказалось. Теперь и она, и её декодеры
# вендорены — этих граблей больше нет.
option(WITH_STATIC_QT "Require static Qt" OFF)

# Вендоренное — только статикой. Без этой строки libjxl объявляет в своём корне
# `option(BUILD_SHARED_LIBS ... ON)`, и он ложится в кэш НА ВСЮ сборку: рядом с
# программой появляются libjxl_cms.so, libjxl_threads.so и три libbrotli*.so —
# причём из КАТАЛОГА СБОРКИ. Установленная программа искала бы их там же, то
# есть `cmake --install` давал бы нерабочий двоичный файл.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Link vendored libs into the application binary (strongly recommended)" FORCE)

add_library(zametti-code-rules INTERFACE)
if(MSVC)
    target_compile_options(zametti-code-rules INTERFACE /W4)
    if(ZAMETTI_WARNINGS_AS_ERRORS)
        target_compile_options(zametti-code-rules INTERFACE /WX)
    endif()
else()
    target_compile_options(zametti-code-rules INTERFACE -Wall -Wextra -Wpedantic)
    if(ZAMETTI_WARNINGS_AS_ERRORS)
        target_compile_options(zametti-code-rules INTERFACE -Werror)
    endif()
endif()

enable_testing()

# Ищем Qt здесь, а не только в подкаталогах: сводке ниже нужна его версия, а
# find_package повторно ничего не стоит. QUIET и без REQUIRED — ядро и без Qt
# собирается (за этим следит набор core-without-qt), а кто без Qt не может,
# тот требует его сам.
# Svg спрашиваем ОТДЕЛЬНО и ПЕРВЫМ. Отдельно — потому что без него собирается
# всё, кроме самой программы (иконки тулбара рисовать нечем), и мешать его в
# общий список значило бы объявить «Qt нет» из-за необязательной части.
# Первым — потому что Qt6_FOUND хранит итог ПОСЛЕДНЕГО вызова, а нам нужен итог
# главного. Здесь же, в корне, а не только в app/: иначе цель Qt6::Svg живёт в
# чужой области видимости, и сводка её не видит (первая редакция так и врала:
# «НЕТ: Svg» при собирающейся программе).
find_package(Qt6 QUIET COMPONENTS Svg)
find_package(Qt6 QUIET COMPONENTS Core Gui Widgets Test)

# ЕСЛИ Qt НЕ НАШЁЛСЯ, СКАЗАТЬ ОБ ЭТОМ ЗДЕСЬ И ВНЯТНО.
#
# Без этого настройка падала глубоко в store/ на голом `find_package(Qt6
# REQUIRED)`, а человек видел только «Configuring incomplete, errors occurred!»
# — без подсказки, чего именно не хватило и куда смотреть. Владелец наткнулся
# на это, собирая на маке.
#
# Ошибкой НЕ объявляем: ядро собирается и без Qt (за этим следит набор
# core-without-qt), а кому Qt нужен, тот требует его сам — и теперь уже с
# подсказкой перед глазами.
if(NOT Qt6_FOUND)
    message(NOTICE "")
    message(NOTICE "Qt6 not found.")
    if(APPLE)
        message(NOTICE "  on macos Qt from brew does not reveal itself; specify the path explicitly:")
        message(NOTICE "    cmake -S . -B build -DCMAKE_PREFIX_PATH=\"$(brew --prefix qt)\"")
    elseif(WIN32)
        message(NOTICE "  specify qt path: cmake -S . -B build -DCMAKE_PREFIX_PATH=C:/Qt/6.x/msvc2022_64")
    else()
        message(NOTICE "  install qt6-base-dev (or specify it with CMAKE_PREFIX_PATH).")
    endif()
    message(NOTICE "")
endif()

# libsodium — крипто-вендор облачной синхронизации (решение владельца, бриф
# m17): XChaCha20-Poly1305 для блобов, Argon2id для пароля. Зависимость
# ЖЁСТКАЯ: шифрование не бывает «необязательной частью», программа без него
# молча обещала бы синк, которого нет.
#
# ВЕНДОРЕННАЯ, а не системная (решение владельца 25.08.2026). Была системной,
# и на чистой машине сборка требовала `apt install libsodium-dev` отдельным
# шагом. Для переносимой статической сборки его всё равно пришлось бы собирать
# самим — против sysroot, а не из системы, — так что вендоринг убирает шаг
# развёртки и ничего не добавляет. Подключается ниже, вместе с остальным
# чужим добром; цель Sodium::Sodium осталась той же, потребители не заметили.

# Динамический Qt или статический. Спрашиваем ЦЕЛЬ, а не переменную
# QT6_IS_SHARED_LIBS_BUILD: та внутренняя, а тип импортированной библиотеки —
# договор CMake, и врать ему нечем.
set(ZAMETTI_QT_SHARED ON)
if(TARGET Qt6::Core)
    get_target_property(zametti_qt_core_type Qt6::Core TYPE)
    if(zametti_qt_core_type STREQUAL "STATIC_LIBRARY")
        set(ZAMETTI_QT_SHARED OFF)
    endif()
endif()

# Ключ WITH_STATIC_QT ничего не переключает — он СТЕРЕЖЁТ. Собрать статически
# можно только тем Qt, который сам собран статически, и если человек просил
# самодостаточную программу, а нашёлся обычный Qt, честнее остановиться, чем
# выдать ему то же, что и всегда.
if(WITH_STATIC_QT AND Qt6_FOUND AND ZAMETTI_QT_SHARED)
    message(FATAL_ERROR
        "WITH_STATIC_QT=ON, but the found Qt ${Qt6_VERSION} is dynamic (${QT6_INSTALL_PREFIX}).\n"
        "  Build Qt with '-static' and provide the path to it via -DCMAKE_PREFIX_PATH.")
endif()

# --- ПЛАГИНЫ СТАТИЧЕСКОГО Qt ----------------------------------------------
#
# У ДИНАМИЧЕСКОГО Qt плагины лежат файлами и подбираются на исполнении: нужен
# offscreen — нашёлся offscreen, сел человек в Wayland — нашёлся wayland. У
# СТАТИЧЕСКОГО их надо вписать в компоновку заранее, и вписываются только те,
# что названы. Автоматика qt_add_executable приносит ОДНУ платформу — ту, что
# по умолчанию (на Linux xcb), и на этом останавливается.
#
# Молчаливость этой пропажи и есть беда. Первая статическая сборка собралась,
# запустилась, показала окно — и упала с SIGABRT на `--check`: «Available
# platform plugins are: xcb». А на `offscreen` стоят и режим `--check`, и все
# до единого наборы. Заодно потерялись бы оба плагина ВВОДА — то есть мёртвые
# клавиши и методы ввода, чего в заметочнике не заметить нельзя.
#
# ПРАВИЛО, по которому собран список: статическая сборка должна вести себя
# так же, как динамическая. Поэтому берём то, что динамический Qt подхватил бы
# сам, а не «то, без чего не падает».
function(zametti_import_qt_plugins target)
    if(ZAMETTI_QT_SHARED)
        return()  # динамический Qt найдёт их сам, вписывать нечего
    endif()

    # Список зависит от системы, и это не «удобство»: плагин, которого на этой
    # платформе нет, ниже отсеется по TARGET молча, а вот НАЗВАТЬ нужное надо
    # здесь — иначе оно молча же и не попадёт.
    set(wanted
        # Общее для всех. offscreen — режим --check и ВСЕ наборы; minimal —
        # запасной аэродром без дисплея.
        Qt6::QOffscreenIntegrationPlugin
        Qt6::QMinimalIntegrationPlugin)

    if(WIN32)
        list(APPEND wanted
            # Платформа. Приходит и по умолчанию, но названа явно: умолчание —
            # не договор, а сегодняшнее поведение qt_add_executable.
            Qt6::QWindowsIntegrationPlugin
            # ОБЛИК ОКНА. С Qt 6.7 стили стали плагинами, и родной стиль
            # Windows — один из них. Без него QStyleFactory отдаёт только
            # Fusion, и программа выглядит чужой на своей же системе — ровно
            # тот класс молчаливой пропажи, ради которого список и заведён.
            #
            # Имён два, потому что Qt его ПЕРЕИМЕНОВАЛА: до 6.10 плагин звался
            # windowsvista, с 6.10 — modernwindows. Лишнее отсеется по TARGET
            # ниже; названы оба, чтобы список не сломался ни на той Qt, ни на
            # этой.
            Qt6::QWindowsVistaStylePlugin
            Qt6::QModernWindowsStylePlugin)
    elseif(APPLE)
        list(APPEND wanted Qt6::QCocoaIntegrationPlugin)
    else()
        list(APPEND wanted
            # Платформы. xcb приходит по умолчанию, остальные — только по имени.
            Qt6::QWaylandIntegrationPlugin          # родной Wayland вместо XWayland
            # Без оболочки окна wayland-плагин не создаёт ОКОН вовсе, а без
            # украшений и EGL остаётся без рамки и без ускорения.
            Qt6::QWaylandXdgShellIntegrationPlugin
            Qt6::QWaylandEglClientBufferPlugin
            # ЗАГОЛОВОК ОКНА. Под Wayland его рисует не оконный менеджер, а сама
            # программа, и Qt выбирает чем: под GNOME она ищет плагин с ключом
            # `adwaita` или `gnome` и, НЕ НАЙДЯ, берёт первый попавшийся — то есть
            # свой запасной bradient с серо-синим градиентом
            # (qwaylandwindow.cpp, createDecoration). Гномовский заголовок живёт в
            # модуле qtwayland, который для программы иначе не нужен вовсе:
            # wayland-клиент в 6.10 переехал в qtbase.
            #
            # Оба плагина вписываются вместе: bradient — запасной, и вне GNOME Qt
            # сама выкидывает adwaita из списка. Под KDE ни тот, ни другой не
            # понадобится: KWin умеет серверные декорации, а поддержка
            # xdg-decoration входит в плагин xdg-shell выше.
            #
            # Ноль новых динамических зависимостей: adwaita просит только
            # Qt::DBus, Qt::Svg и Wayland::Client, а libdbus-1 и Qt6Svg в
            # программе уже есть.
            Qt6::QWaylandAdwaitaDecorationPlugin
            Qt6::QWaylandBradientDecorationPlugin
            # Ввод. compose — мёртвые клавиши и Compose; ibus — методы ввода.
            Qt6::QComposePlatformInputContextPlugin
            Qt6::QIbusPlatformInputContextPlugin
            # Родные диалоги файлов через портал — так же, как с динамическим Qt.
            Qt6::QXdgDesktopPortalThemePlugin)
    endif()

    set(found "")
    foreach(plugin ${wanted})
        if(TARGET ${plugin})
            list(APPEND found ${plugin})
        endif()
    endforeach()

    # Пусто — значит Qt собран без них; сказать об этом, а не промолчать.
    if(NOT found)
        message(WARNING
            "статический Qt без запрошенных плагинов — режим --check и наборы "
            "останутся без offscreen и упадут")
        return()
    endif()
    qt_import_plugins(${target} INCLUDE ${found})
endfunction()

# --- МАНИФЕСТ WINDOWS -----------------------------------------------------
#
# Один ресурс на все исполняемые файлы проекта, и в нём одна работающая
# строка — activeCodePage=UTF-8. Зачем именно она, подробно написано в самом
# packaging/win/zametti.manifest; коротко: без неё узкая половина API Windows
# читает наши UTF-8-байты как CP1251/CP1252 и молча съедает кириллицу.
#
# Ставится ВСЕМ исполняемым файлам, а не только программе: наборы, пробники и
# mddump ходят по тем же путям и ломались бы ровно так же (и ломались —
# одиннадцать наборов краснели из-за каталога «наборы», ставшего «??????»).
if(WIN32)
    set(ZAMETTI_WIN_MANIFEST "${CMAKE_SOURCE_DIR}/packaging/win/zametti.manifest")
    # Значок .exe — готовый .ico из репозитория, а не собираемый сборкой:
    # кросс-сборке негде взять хостовую программу, которая его соберёт. Чем он
    # сделан — resources/make-ico.cpp, там же и формат разобран.
    set(ZAMETTI_WIN_ICO "${CMAKE_SOURCE_DIR}/resources/zametti.ico")
    if(NOT EXISTS "${ZAMETTI_WIN_ICO}")
        message(FATAL_ERROR
            "нет ${ZAMETTI_WIN_ICO} — программа собралась бы без значка, и\n"
            "  заметить это можно было бы только глазами в проводнике.\n"
            "  Собрать: см. шапку resources/make-ico.cpp")
    endif()
    configure_file(${CMAKE_SOURCE_DIR}/packaging/win/zametti.rc.in
                   ${CMAKE_BINARY_DIR}/generated/zametti.rc @ONLY)
    enable_language(RC)
endif()

# Дописывает манифест целевому исполняемому файлу. Вне Windows — ничего не
# делает, чтобы зовущим не приходилось оборачивать вызов в if().
function(zametti_windows_manifest target)
    if(WIN32)
        target_sources(${target} PRIVATE ${CMAKE_BINARY_DIR}/generated/zametti.rc)
    endif()
endfunction()

# --- СКОРОСТЬ ОТЛАДОЧНОЙ СБОРКИ (решение владельца) ------------------------
#
# Правило владельца: отладку мы гоняем ради СВОИХ ассертов, а не ради чужих.
# Кодеки картинок, zstd, highway и прочее проверены суровым продакшном; собирать
# их без оптимизации — жечь время на каждом прогоне набора и ничего за это не
# получать. Поэтому:
#
#   чужое, кроме microtex   —  всегда -O3, и в отладочной сборке тоже;
#   наше и microtex         —  в отладке -O1.
#
# microtex в исключении назван прямо: это библиотека энтузиаста, она у нас под
# подозрением (см. zametti-formulas-state), и отлаживать её мы будем.
#
# -O1, а не -O0: под ноль сборка идёт заметно дольше, а разница в отладке
# невелика. Ассерты нигде не гасятся — NDEBUG не появляется ни у кого, и Q_ASSERT
# в отладке остаётся на месте: тип сборки этим ключом не подменяется.
if(NOT MSVC)
    set(ZAMETTI_DEBUG_C_FLAGS "${CMAKE_C_FLAGS_DEBUG}")
    set(ZAMETTI_DEBUG_CXX_FLAGS "${CMAKE_CXX_FLAGS_DEBUG}")
    set(CMAKE_C_FLAGS_DEBUG "${ZAMETTI_DEBUG_C_FLAGS} -O3")
    set(CMAKE_CXX_FLAGS_DEBUG "${ZAMETTI_DEBUG_CXX_FLAGS} -O3")
endif()

add_subdirectory(3rdparty/md4c)
add_subdirectory(3rdparty/blake3)
add_subdirectory(3rdparty/dtl)
# highway раньше libjxl: та её ищет, а не несёт.
add_subdirectory(3rdparty/zlib)
add_subdirectory(3rdparty/libtiff)
add_subdirectory(3rdparty/highway)
add_subdirectory(3rdparty/zstd)
add_subdirectory(3rdparty/libjxl)
add_subdirectory(3rdparty/jpegli)
add_subdirectory(3rdparty/libwebp)
# AVIF и HEIC. Порядок ЖЁСТКИЙ: libheif ищет libde265 чужим FindLIBDE265, и мы
# подсовываем ему цель по имени — она обязана уже существовать. Сторожа стоят
# в самом 3rdparty/libheif/CMakeLists.txt.
add_subdirectory(3rdparty/libde265)
add_subdirectory(3rdparty/libgav1)
add_subdirectory(3rdparty/libheif)
add_subdirectory(3rdparty/libsodium)
add_subdirectory(3rdparty/gtest)

# Дальше — то, что мы отлаживаем: microtex и всё своё.
if(NOT MSVC)
    set(CMAKE_C_FLAGS_DEBUG "${ZAMETTI_DEBUG_C_FLAGS} -O1")
    set(CMAKE_CXX_FLAGS_DEBUG "${ZAMETTI_DEBUG_CXX_FLAGS} -O1")
endif()

add_subdirectory(3rdparty/microtex)
add_subdirectory(zametti-core)
add_subdirectory(app)
add_subdirectory(tests)

# --- сводка ---------------------------------------------------------------
#
# Печатается при каждой настройке: с чем именно собираемся, видно сразу, а не
# после раскопок в CMakeCache. Здесь, в самом конце, — к этому месту известно
# всё, включая то, что завели подкаталоги.
#
# message(NOTICE), а не STATUS: тот дописывает "-- " к каждой строке и разломал
# бы рамку.

function(zametti_row label value)
    # Русские подписи считаются в БАЙТАХ, а рисуются знаками: string(LENGTH)
    # для выравнивания не годится вовсе. Поэтому ширину держим пробелами прямо
    # в подписи, а здесь только склеиваем.
    message(NOTICE "* ${label} ${value}")
endfunction()

# Вложенный пункт списка: вендоренного добра накопилось столько, что в строку
# оно уже не влезает.
function(zametti_subrow value)
    message(NOTICE "    * ${value}")
endfunction()

function(zametti_summary)
    set(platform "${CMAKE_SYSTEM_NAME} ${CMAKE_SYSTEM_VERSION} (${CMAKE_SYSTEM_PROCESSOR})")

    if(CMAKE_CONFIGURATION_TYPES)
        set(config "${CMAKE_CONFIGURATION_TYPES} (выбор при сборке)")
    else()
        set(config "${CMAKE_BUILD_TYPE}")
        # Флаги именно этой конфигурации: пустой CMAKE_CXX_FLAGS сбивает с толку,
        # когда вся разница между Debug и Release живёт в _DEBUG/_RELEASE.
        string(TOUPPER "${CMAKE_BUILD_TYPE}" upper)
        set(flags "${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_${upper}}")
        string(STRIP "${flags}" flags)
        if(flags)
            set(config "${config}   ${flags}")
        endif()
    endif()

    set(compiler "${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}, C++${CMAKE_CXX_STANDARD}")

    if(Qt6_FOUND)
        # Статический он или нет — не мелочь оформления: от этого зависит,
        # останется ли рядом с программой чужое добро.
        if(ZAMETTI_QT_SHARED)
            set(qt "${Qt6_VERSION} dynamic")
        else()
            set(qt "${Qt6_VERSION} static")
        endif()
        set(missing "")
        # Спрашиваем ЦЕЛЬ, а не переменную Qt6<Часть>_FOUND: ту выставляет
        # find_package в СВОЕЙ области видимости, и Svg, найденный в app/, в
        # корне так и остался бы «ненайденным» (первая же редакция этой строки
        # соврала именно так).
        foreach(part Core Gui Widgets Svg Test)
            if(NOT TARGET Qt6::${part})
                list(APPEND missing ${part})
            endif()
        endforeach()
        if(missing)
            string(REPLACE ";" ", " missing "${missing}")
            set(qt "${qt}   not found: ${missing}")
        endif()
    else()
        set(qt "not found")
    endif()

    if(ZAMETTI_CORPUS)
        string(REPLACE ";" ", " corpora "${ZAMETTI_CORPUS}")
        set(fuzz "mutations ${ZAMETTI_CORPUS_MUTATIONS}, rounds ${ZAMETTI_FUZZ_ROUNDS}, seed ${ZAMETTI_FUZZ_SEED}")
    else()
        set(corpora "fuzzers and tests are skipped")
        set(fuzz "")
    endif()

    if(ZAMETTI_WARNINGS_AS_ERRORS)
        set(strict "warnings are errors")
    else()
        set(strict "warnings are not errors; change with -DZAMETTI_WARNINGS_AS_ERRORS=ON")
    endif()

    message(NOTICE "")
    message(NOTICE "___")
    message(NOTICE "")
    message(NOTICE "# Zametti configuration")
    message(NOTICE "")
    zametti_row("platform: " "`${platform}`")
    zametti_row("config:    " "`${config}`")
    zametti_row("compiler:" "`${compiler}`")
    zametti_row("Qt:        " "`${qt}`")
    zametti_row("vendored libraries: " "")
    zametti_subrow("`md4c ${MD4C_VENDORED_VERSION}` — markdown parsing")
    zametti_subrow("`blake3 ${BLAKE3_VENDORED_VERSION}` — computing digests of notes and attachments (images)")
    zametti_subrow("`zstd ${ZSTD_VENDORED_VERSION}` — journal compression")
    zametti_subrow("`dtl ${DTL_VENDORED_VERSION}` — compute difference between note' revisions")
    zametti_subrow("`zlib ${ZLIB_VENDORED_VERSION}` — deflate in TIFF, just for libtiff")
    zametti_subrow("`libtiff ${TIFF_VENDORED_VERSION}` — TIFF codec with Lab/CMYK, ICC, XMP, EXIF support")
    zametti_subrow("`highway ${HWY_VENDORED_VERSION}` — SIMD library for libjxl and jpegli")
    zametti_subrow("`libjxl ${JXL_VENDORED_VERSION}` — JpegXL codec, JXL <=> JPEG transcoder, SSIMULACRA2")
    zametti_subrow("`jpegli ${JPEGLI_VENDORED_COMMIT}` — high-fidelity JPEG codec")
    zametti_subrow("`libwebp ${LIBWEBP_VENDORED_VERSION}` — WebP decoder")
    zametti_subrow("`libheif ${HEIF_VENDORED_VERSION}` — HEIF/AVIF container")
    zametti_subrow("`libde265 ${DE265_VENDORED_VERSION}` — HEVC decoder inside HEIC")
    zametti_subrow("`libgav1 ${LIBGAV1_VENDORED_VERSION}` — AV1 decoder inside AVIF")
    zametti_subrow("`libsodium ${SODIUM_VENDORED_VERSION}` — XChaCha20-Poly1305 and Argon2id for the cloud sync")
    zametti_subrow("`googletest ${GTEST_VENDORED_VERSION}` — unit test engine")
    set(readers "`avif`/`heic`/`heif` — vendored, no system libraries needed")
    zametti_row("readers:   " "${readers}")
    # Есть ли сама программа. Строка обязательна: без неё «настройка прошла»
    # означало лишь, что не упало, — а собралось при этом одно ядро.
    if(TARGET zametti)
        zametti_row("the program: " "`build`")
    else()
        zametti_row("the program: " "`SKIP` — ${ZAMETTI_APP_SKIPPED}")
    endif()
    zametti_row("tests:   " "`${corpora}`")
    if(fuzz)
        zametti_row("fuzzing:   " "`${fuzz}`")
    endif()
    zametti_row("strictness: " "`${strict}`")
    message(NOTICE "___")
    message(NOTICE "")

    # Те же строки уезжают в окно «О программе». Именно эти, а не собранные
    # заново рядом: два списка версий разошлись бы первым же обновлением
    # вендоринга, и окно врало бы с уверенным видом.
    set(ZAMETTI_FACT_PLATFORM "${platform}" PARENT_SCOPE)
    set(ZAMETTI_FACT_CONFIG "${config}" PARENT_SCOPE)
    set(ZAMETTI_FACT_COMPILER "${compiler}" PARENT_SCOPE)
    set(ZAMETTI_FACT_QT "${qt}" PARENT_SCOPE)
    string(REPLACE "`" "" plain_readers "${readers}")
    set(ZAMETTI_FACT_READERS "${plain_readers}" PARENT_SCOPE)
endfunction()

zametti_summary()

# Отметка коммита — на момент НАСТРОЙКИ. Снимать её при каждой сборке значило бы
# трогать заголовок и пересобирать за ним весь app; в окне так и написано —
# «настроено», а не «собрано».
find_package(Git QUIET)
set(ZAMETTI_FACT_COMMIT "unknown")
if(GIT_FOUND)
    execute_process(COMMAND ${GIT_EXECUTABLE} -C ${CMAKE_SOURCE_DIR} rev-parse --short HEAD
                    OUTPUT_VARIABLE git_commit OUTPUT_STRIP_TRAILING_WHITESPACE
                    ERROR_QUIET RESULT_VARIABLE git_failed)
    if(NOT git_failed AND git_commit)
        set(ZAMETTI_FACT_COMMIT "${git_commit}")
    endif()
endif()
string(TIMESTAMP ZAMETTI_FACT_CONFIGURED "%Y-%m-%d %H:%M")
configure_file(${CMAKE_SOURCE_DIR}/app/build_facts.h.in
               ${CMAKE_BINARY_DIR}/generated/build_facts.h @ONLY)
