# libgav1 0.20.0, вендоринг чужим CMake. Взято из
# https://chromium.googlesource.com/codecs/libgav1, тег v0.20.0; чем именно
# обрезано дерево — см. update.sh рядом.
#
# ЗАЧЕМ ОН НАМ. Это декодер AV1 — то, чем разжимается AVIF. libheif разбирает
# контейнер, а кодек зовёт со стороны.
#
# ПОЧЕМУ libgav1, А НЕ libaom (решение владельца 25.08.2026). У libaom 24 файла
# ассемблера, и единственный способ обойтись без ассемблера —
# AOM_TARGET_CPU=generic, который гасит ЗАОДНО И ИНТРИНЗИКИ. Замер на
# avif-sample.avif (1792×1024, taskset -c 2, три прогона): 0.13 0.13 0.13 с с
# векторизацией против 0.30 0.30 0.30 с без неё — 2.3 раза. У libgav1 файлов
# ассемблера НОЛЬ, вся векторизация интринзиками, и ассемблер не нужен ни на
# одной из трёх платформ, куда мы идём. Дерево вдвое с лишним меньше: 5.9 МБ
# против 12.3 МБ обрезанного libaom.
#
# ПОЧЕМУ ТЕГ, А НЕ main. На main в июне 2026 появился CheckLevelConformance(),
# который может забраковать 102-мегапиксельный кадр из корпуса владельца как
# выходящий за уровень AV1. В v0.20.0 этой проверки нет.
#
# ЧЕГО ЗДЕСЬ НЕТ: libheif про libgav1 не знает вовсе (в её libheif/plugins/
# лежат только aom, dav1d, ffmpeg и webcodecs). Декодер-плагин для неё —
# наш собственный код, см. zametti-core/image/.

set(gav1_dir "${CMAKE_CURRENT_SOURCE_DIR}/upstream")

# Версия — из чужого заголовка, а не вписанная руками.
file(READ "${gav1_dir}/src/gav1/version.h" gav1_version_h)
string(REGEX MATCH "#define LIBGAV1_MAJOR_VERSION ([0-9]+)" _m "${gav1_version_h}")
set(_gav1_major "${CMAKE_MATCH_1}")
string(REGEX MATCH "#define LIBGAV1_MINOR_VERSION ([0-9]+)" _m "${gav1_version_h}")
set(_gav1_minor "${CMAKE_MATCH_1}")
string(REGEX MATCH "#define LIBGAV1_PATCH_VERSION ([0-9]+)" _m "${gav1_version_h}")
set(_gav1_patch "${CMAKE_MATCH_1}")
if(_gav1_major STREQUAL "" OR _gav1_minor STREQUAL "" OR _gav1_patch STREQUAL "")
    message(FATAL_ERROR
        "libgav1: не удалось прочитать версию из upstream/src/gav1/version.h.\n"
        "  Похоже, обновление сменило формат — поправьте регулярку здесь.")
endif()
set(LIBGAV1_VENDORED_VERSION "${_gav1_major}.${_gav1_minor}.${_gav1_patch}" PARENT_SCOPE)

# ЭТА СТРОКА УБИРАЕТ ЗАВИСИМОСТЬ ОТ ABSEIL ЦЕЛИКОМ. Единственное место с absl —
# src/utils/threadpool.h, и оно уходит на std::mutex. Без неё чужой CMake
# падает `FATAL_ERROR "Abseil not found"` и требует склонировать абсейл внутрь
# своего дерева.
#
# CACHE STRING, А НЕ BOOL, И ЭТО НЕ ПРИДИРКА: чужой код сверяет значение через
# `EQUAL 1`, и слово ON проверку не проходит.
set(LIBGAV1_THREADPOOL_USE_STD_MUTEX 1 CACHE STRING "" FORCE)

# Двенадцать бит нужны: в корпусе владельца есть 12-битные AVIF, и на них
# лежат эталоны. Совпадает с чужим умолчанием, но записать причину надо здесь —
# больше её записать негде.
set(LIBGAV1_MAX_BITDEPTH 12 CACHE STRING "" FORCE)

set(LIBGAV1_ENABLE_OPTIMIZATIONS ON  CACHE BOOL "" FORCE)
set(LIBGAV1_ENABLE_SSE4_1        ON  CACHE BOOL "" FORCE)
set(LIBGAV1_ENABLE_AVX2          ON  CACHE BOOL "" FORCE)
set(LIBGAV1_ENABLE_NEON          ON  CACHE BOOL "" FORCE)
set(LIBGAV1_ENABLE_EXAMPLES      OFF CACHE BOOL "" FORCE)
set(LIBGAV1_ENABLE_TESTS         OFF CACHE BOOL "" FORCE)
set(LIBGAV1_VERBOSE              OFF CACHE BOOL "" FORCE)

# PIC чужой CMake включает ТОЛЬКО при BUILD_SHARED_LIBS, а он у нас
# принудительно OFF на всю сборку. Обычная переменная области каталога
# наследуется в add_subdirectory и накрывает все цели libgav1 разом.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

add_subdirectory(upstream EXCLUDE_FROM_ALL)

# --- СТОРОЖ ВЕКТОРИЗАЦИИ ---------------------------------------------------
#
# Ради чего он: src/dsp/x86/cdef_sse4.cc и его двадцать один сосед целиком
# обёрнуты в `#if LIBGAV1_TARGETING_SSE4_1`, а под `#else` лежит ПУСТАЯ
# ЗАГЛУШКА вида `void CdefInit_SSE4_1() {}`. Макрос зависит от __SSE4_1__, то
# есть от того, достался ли ЭТОМУ файлу ключ -msse4.1. Файл без ключа
# компилируется в ничто — молча, без единого предупреждения, и библиотека
# просто оказывается медленнее. Ровно тот класс отказа, ради которого мы ушли
# от libaom, и единственный способ узнать о нём — считать.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|i[3-6]86)$")
    foreach(pair "_sse4.cc;msse4" "_avx2.cc;mavx2")
        list(GET pair 0 gav1_suffix)
        list(GET pair 1 gav1_flag)
        file(GLOB gav1_simd "${gav1_dir}/src/dsp/x86/*${gav1_suffix}")
        list(LENGTH gav1_simd gav1_total)
        set(gav1_flagged 0)
        foreach(gav1_src ${gav1_simd})
            get_source_file_property(gav1_opts "${gav1_src}"
                                     DIRECTORY "${gav1_dir}" COMPILE_FLAGS)
            if(gav1_opts MATCHES "${gav1_flag}")
                math(EXPR gav1_flagged "${gav1_flagged} + 1")
            endif()
        endforeach()
        if(NOT gav1_flagged EQUAL gav1_total)
            message(FATAL_ERROR
                "libgav1: ключ -${gav1_flag} достался ${gav1_flagged} файлам "
                "*${gav1_suffix} из ${gav1_total}.\n"
                "  Файл без ключа компилируется в ПУСТУЮ ЗАГЛУШКУ, и декодер "
                "молча теряет векторизацию.\n"
                "  Смотрите upstream/cmake/libgav1_intrinsics.cmake: там ключи "
                "раздаются по суффиксу имени файла.")
        endif()
        message(STATUS "libgav1: -${gav1_flag} на всех ${gav1_total} файлах *${gav1_suffix}")
    endforeach()
endif()

# Одна цель наружу.
#
# Threads::Threads — НЕ КОСМЕТИКА, А ПОЧИНКА. src/utils/threadpool.cc зовёт
# pthread_create и pthread_join напрямую, а чужой CMake добавляет нити только
# исполняемым файлам, не самой библиотеке. На этой машине (glibc 2.34+, где
# pthread внутри libc) это незаметно; в переносимой сборке против sysroot
# Ubuntu 20.04 компоновка развалилась бы. Классическое «здесь работает, там
# нет».
find_package(Threads REQUIRED)
add_library(libgav1 INTERFACE)
target_link_libraries(libgav1 INTERFACE libgav1_static Threads::Threads)
target_include_directories(libgav1 SYSTEM INTERFACE "${gav1_dir}/src")

# Чужой код чужими правилами: zametti-code-rules ему не цепляется.
