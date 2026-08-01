cmake_minimum_required(VERSION 3.20)
project(zametti LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 99)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "" FORCE)
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(ZAMETTI_WARNINGS_AS_ERRORS "Считать предупреждения ошибками" OFF)

add_library(zametti-warnings INTERFACE)
if(MSVC)
    target_compile_options(zametti-warnings INTERFACE /W4)
    if(ZAMETTI_WARNINGS_AS_ERRORS)
        target_compile_options(zametti-warnings INTERFACE /WX)
    endif()
else()
    target_compile_options(zametti-warnings INTERFACE -Wall -Wextra -Wpedantic)
    if(ZAMETTI_WARNINGS_AS_ERRORS)
        target_compile_options(zametti-warnings INTERFACE -Werror)
    endif()
endif()

enable_testing()

# Ищем Qt здесь, а не только в подкаталогах: сводке ниже нужна его версия, а
# find_package повторно ничего не стоит. QUIET и без REQUIRED — ядро и без Qt
# собирается (за этим следит набор core-without-qt), а кто без Qt не может,
# тот требует его сам.
find_package(Qt6 QUIET COMPONENTS Core Gui Widgets Test)

add_subdirectory(3rdparty/md4c)
add_subdirectory(3rdparty/blake3)
# highway раньше libjxl: та её ищет, а не несёт.
add_subdirectory(3rdparty/highway)
add_subdirectory(3rdparty/zstd)
add_subdirectory(3rdparty/libjxl)
add_subdirectory(3rdparty/jpegli)
add_subdirectory(zametti-core)
add_subdirectory(imageio)
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
        set(qt "${Qt6_VERSION}")
        set(missing "")
        foreach(part Core Gui Widgets Test)
            if(NOT Qt6${part}_FOUND)
                list(APPEND missing ${part})
            endif()
        endforeach()
        if(missing)
            string(REPLACE ";" ", " missing "${missing}")
            set(qt "${qt}   НЕТ: ${missing}")
        endif()
    else()
        set(qt "не найден — соберётся только ядро")
    endif()

    if(ZAMETTI_CORPUS)
        string(REPLACE ";" ", " corpora "${ZAMETTI_CORPUS}")
        set(fuzz "мутаций ${ZAMETTI_CORPUS_MUTATIONS}, правок ${ZAMETTI_FUZZ_ROUNDS}, зерно ${ZAMETTI_FUZZ_SEED}")
    else()
        set(corpora "нет — фаззеры и корпусные наборы не заводятся")
        set(fuzz "")
    endif()

    if(ZAMETTI_WARNINGS_AS_ERRORS)
        set(strict "предупреждения = ошибки")
    else()
        set(strict "предупреждения не ошибки; включить: -DZAMETTI_WARNINGS_AS_ERRORS=ON")
    endif()

    message(NOTICE "")
    message(NOTICE "___")
    message(NOTICE "")
    message(NOTICE "# Zametti configuration")
    message(NOTICE "")
    zametti_row("платформа: " "`${platform}`")
    zametti_row("сборка:    " "`${config}`")
    zametti_row("компилятор:" "`${compiler}`")
    zametti_row("Qt:        " "`${qt}`")
    zametti_row("вендоринг: " "")
    zametti_subrow("`md4c ${MD4C_VENDORED_VERSION}` — разбор markdown")
    zametti_subrow("`blake3 ${BLAKE3_VENDORED_VERSION}` — отпечатки заметок и вложений")
    zametti_subrow("`zstd ${ZSTD_VENDORED_VERSION}` — слепки в журнале правок")
    zametti_subrow("`highway ${HWY_VENDORED_VERSION}` — SIMD, общая для libjxl и jpegli")
    zametti_subrow("`libjxl ${JXL_VENDORED_VERSION}` — картинки: кодек, транскод JPEG, SSIMULACRA2")
    zametti_subrow("`jpegli ${JPEGLI_VENDORED_COMMIT}` — разжатие JPEG в 16 бит при импорте")
    if(ZAMETTI_HAVE_HEIF)
        set(readers "`jxl` (своя libjxl), `avif`/`heic`/`heif` (системная libheif ${ZAMETTI_HEIF_VERSION})")
    elseif(Qt6_FOUND)
        set(readers "`jxl` (своя libjxl); avif/heic НЕ читаются — нет libheif-dev")
    else()
        set(readers "не собираются — нет Qt")
    endif()
    zametti_row("читатели:  " "${readers}")
    zametti_row("корпуса:   " "`${corpora}`")
    if(fuzz)
        zametti_row("фаззинг:   " "`${fuzz}`")
    endif()
    zametti_row("строгость: " "`${strict}`")
    message(NOTICE "___")
    message(NOTICE "")
endfunction()

zametti_summary()
