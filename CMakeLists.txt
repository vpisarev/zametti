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

# AVIF и HEIC. ВЫКЛЮЧЕНО по умолчанию, и это перемена: раньше libheif бралась
# молча, если нашлась. Причина в том, что она не одна: за ней идут видеокодеки
# (aom либо de265/x265), а с ними — их зависимости. Пока сборка динамическая,
# это чужая забота; в статической каждый из них нужен отдельным архивом, и
# владелец на маке наступил ровно на это. AVIF/HEIC у нас формат ВХОДА, не
# хранения: без них программа теряет ввоз двух форматов и ничего больше.
option(WITH_HEIF "Support AVIF и HEIC (libheif-dev is required then)" OFF)

# Статический Qt НЕ включается ключом — он либо есть на машине, либо нет:
# динамический Qt статическим не станет ни от каких флагов. Ключ означает
# другое: «я собираю ради самодостаточной программы, и молча получить
# динамический Qt не согласен». Без него сборка одинаково рада обоим.
#
# Именно здесь владелец обжёгся: совет из сети предлагал добавить -static в
# флаги компоновки, и статическими стали заодно libheif с кодеками, которых в
# таком виде на машине не оказалось.
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
# ЖЁСТКАЯ и СИСТЕМНАЯ: не вендорим (повсеместен в Ubuntu и brew), но и
# собраться без него нельзя — шифрование не бывает «необязательной частью»,
# программа без него молча обещала бы синк, которого нет. Поэтому не QUIET и
# остановка здесь, с подсказкой, а не глубоко в zametti-core на голом линке.
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
find_package(Sodium)
if(NOT Sodium_FOUND)
    if(APPLE)
        set(sodium_hint "brew install libsodium")
    elseif(WIN32)
        set(sodium_hint "vcpkg install libsodium (or provide it via CMAKE_PREFIX_PATH)")
    else()
        set(sodium_hint "sudo apt install libsodium-dev")
    endif()
    message(FATAL_ERROR
        "libsodium not found — it encrypts the notes for the cloud sync.\n"
        "  ${sodium_hint}")
endif()

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
add_subdirectory(3rdparty/gtest)

# Дальше — то, что мы отлаживаем: microtex и всё своё.
if(NOT MSVC)
    set(CMAKE_C_FLAGS_DEBUG "${ZAMETTI_DEBUG_C_FLAGS} -O1")
    set(CMAKE_CXX_FLAGS_DEBUG "${ZAMETTI_DEBUG_CXX_FLAGS} -O1")
endif()

add_subdirectory(3rdparty/microtex)
# imageio раньше ядра: там заводится необязательная цель zametti-heif, и ядро
# спрашивает о ней через if(TARGET ...).
add_subdirectory(imageio)
add_subdirectory(zametti-core)
add_subdirectory(store)
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
        set(fuzz "mutaions ${ZAMETTI_CORPUS_MUTATIONS}, rounds ${ZAMETTI_FUZZ_ROUNDS}, seed ${ZAMETTI_FUZZ_SEED}")
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
    # libsodium — единственная жёсткая системная библиотека помимо Qt; до этой
    # строки настройка доходит только с ним (иначе FATAL_ERROR выше), так что
    # здесь всегда версия — но напечатать её обязательно: она системная, и на
    # разных машинах разная.
    zametti_row("crypto:   " "`libsodium ${Sodium_VERSION} (system)` — cloud encryption: XChaCha20-Poly1305, Argon2id")
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
    zametti_subrow("`googletest ${GTEST_VENDORED_VERSION}` — unit test engine")
    if(ZAMETTI_HAVE_HEIF)
        set(readers "`avif`/`heic`/`heif` (system libheif ${ZAMETTI_HEIF_VERSION})")
    elseif(Qt6_FOUND)
        set(readers "avif/heic are not supported. To enable avif/heic, reconfigure with -DWITH_HEIF=ON`")
    else()
        set(readers "not supported without Qt")
    endif()
    zametti_row("читатели:  " "${readers}")
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
    set(ZAMETTI_FACT_SODIUM "libsodium ${Sodium_VERSION} (system)" PARENT_SCOPE)
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
