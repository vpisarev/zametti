# libde265 1.1.1, вендоринг чужим CMake. Взято из
# https://github.com/strukturag/libde265, тег v1.1.1; чем именно обрезано
# дерево — см. update.sh рядом.
#
# ЗАЧЕМ ОН НАМ. Это декодер HEVC — то, чем разжимается HEIC. libheif сама
# ничего не декодирует: она разбирает контейнер ISOBMFF и зовёт кодек. Без
# de265 из неё получается библиотека, которая честно отказывает всем heic
# подряд, а в корпусе владельца их четырнадцать.
#
# ТОЛЬКО ЧТЕНИЕ. Энкодер (enc265) у upstream есть и по умолчанию выключен;
# включать нам его незачем — пишем мы один формат, JPEG XL.
#
# ПОЧЕМУ ЧУЖОЙ CMake, А НЕ СВОЙ СПИСОК ФАЙЛОВ. Та же причина, что у libwebp и
# libtiff: libde265 сам пробует расширения процессора и раздаёт файлам
# SIMD-диспетчера свои ключи (-msse4.1, -mavx2, -mavx512f по отдельности на
# libde265/x86/*.cc). Писать это руками значит заново делать чужую работу и
# ошибаться в ней на каждой новой машине.

set(de265_dir "${CMAKE_CURRENT_SOURCE_DIR}/upstream")

# Версия — из чужого project(), а не вписанная руками: вписанная разъезжается
# с деревом на первом же обновлении. Тем же приёмом живёт libsodium.
file(READ "${de265_dir}/CMakeLists.txt" de265_root_cmake)
string(REGEX MATCH "project *\\( *libde265[^)]*VERSION[ \t\r\n]+([0-9]+\\.[0-9]+\\.[0-9]+)"
       _de265_match "${de265_root_cmake}")
set(DE265_VENDORED_VERSION "${CMAKE_MATCH_1}")
if(NOT DE265_VENDORED_VERSION)
    message(FATAL_ERROR
        "libde265: не удалось прочитать версию из upstream/CMakeLists.txt.\n"
        "  Похоже, обновление сменило формат project() — поправьте регулярку здесь.")
endif()
set(DE265_VENDORED_VERSION "${DE265_VENDORED_VERSION}" PARENT_SCOPE)

# ОСТОРОЖНО: ИМЯ ВРЁТ. ENABLE_DECODER включает не декодер, а утилиту
# командной строки dec265 (upstream/CMakeLists.txt: `if (ENABLE_DECODER)
# add_subdirectory (dec265)`). Сама библиотека собирается безусловно строкой
# выше. Не «чините» эту строку обратно на ON.
set(ENABLE_DECODER   OFF CACHE BOOL "" FORCE)
set(ENABLE_ENCODER   OFF CACHE BOOL "" FORCE)
set(ENABLE_SHERLOCK265 OFF CACHE BOOL "" FORCE)
set(ENABLE_INTERNAL_DEVELOPMENT_TOOLS OFF CACHE BOOL "" FORCE)
set(WITH_FUZZERS     OFF CACHE BOOL "" FORCE)
set(USE_IWYU         OFF CACHE BOOL "" FORCE)
set(BUILD_FRAMEWORK  OFF CACHE BOOL "" FORCE)
set(FORCE_FULL_VISIBILITY OFF CACHE BOOL "" FORCE)

# SDL гасим не ради размера, а ради sysroot: с ON чужой CMake делает
# find_package(SDL2), и в обычной сборке он возьмёт хостовую SDL2, а в
# переносимой полезет искать её там, где ей быть не положено. Гасить надо сам
# вызов, а не его последствия.
set(ENABLE_SDL       OFF CACHE BOOL "" FORCE)

# Векторизация — чужими умолчаниями: диспетчер проверяет процессор на
# исполнении, поэтому AVX2 и AVX-512 не сужают круг машин.
set(ENABLE_SIMD      ON  CACHE BOOL "" FORCE)
set(ENABLE_AVX2      ON  CACHE BOOL "" FORCE)
set(ENABLE_AVX512    ON  CACHE BOOL "" FORCE)

add_subdirectory(upstream EXCLUDE_FROM_ALL)

# ЧУЖАЯ ЦЕЛЬ НЕ ОТДАЁТ НИ ОДНОГО ЗАГОЛОВКА: у upstream все
# target_include_directories(de265 ...) стоят PRIVATE. То есть «прилинковать
# de265 и включить <libde265/de265.h>» само по себе не работает — каталоги
# обязаны прийти отсюда. Их два, и второй не очевиден: de265.h включает
# <libde265/de265-version.h>, а тот ПОРОЖДАЕТСЯ configure_file в каталоге
# сборки.
#
# $<BUILD_INTERFACE:> обязателен: upstream делает install(TARGETS de265 EXPORT
# libde265-config), а экспортируемой цели CMake запрещает нести в
# INTERFACE_INCLUDE_DIRECTORIES голые пути из дерева исходников и сборки —
# настройка падает на шаге Generate. Ставить его нам ничего не стоит: чужую
# цель мы никуда не устанавливаем, она живёт только внутри нашей сборки.
set(de265_generated "${CMAKE_CURRENT_BINARY_DIR}/upstream")
target_include_directories(de265 SYSTEM INTERFACE
                           "$<BUILD_INTERFACE:${de265_dir}>"
                           "$<BUILD_INTERFACE:${de265_generated}>")

# Без этого макроса LIBDE265_API разворачивается в __declspec(dllimport), и
# статическая компоновка под Windows не складывается. У upstream он
# раздаётся через add_definitions() внутри своего дерева, а потребитель
# (decoder_libde265.cc живёт в дереве libheif) его не видит. На Linux
# безразлично; чиним здесь, чтобы не искать это при заходе на MSVC.
target_compile_definitions(de265 INTERFACE LIBDE265_STATIC_BUILD)

# Путь к сгенерированным заголовкам нужен соседу: 3rdparty/libheif подсовывает
# его чужому FindLIBDE265 как LIBDE265_INCLUDE_DIR — оттуда тот читает номер
# версии из de265-version.h.
set(DE265_GENERATED_INCLUDE_DIR "${de265_generated}" PARENT_SCOPE)

# Чужой код чужими правилами: zametti-code-rules ему не цепляется.
