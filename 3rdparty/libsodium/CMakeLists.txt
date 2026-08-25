# libsodium 1.0.20, вендоринг СВОИМ CMake. Взято из
# https://github.com/jedisct1/libsodium, тег 1.0.20-RELEASE; чем именно
# обрезано дерево — см. update.sh рядом.
#
# ПОЧЕМУ СВОЙ, А НЕ ЧУЖОЙ. Чужого нет: upstream собирается autotools, а под
# Windows — отдельными проектами MSVC. Прецедент в дереве есть: blake3, zlib,
# zstd, md4c, dtl и jpegli тоже собираются нашим CMake.
#
# ГЛАВНОЕ ПРО ЭТУ СБОРКУ. В 1.0.20 расширения процессора включаются НЕ ключами
# компилятора, а `#pragma GCC target(...)` прямо в исходниках. Раздавать здесь
# -mavx2 не нужно и ВРЕДНО: ключ действовал бы на весь файл, а прагма — только
# на нужные функции. От нас требуется одно: честно определить макросы HAVE_*.
# Они работают ВЫКЛЮЧАТЕЛЯМИ — исходник спрашивает макрос и, не увидев, молча
# берёт портируемый путь на C. Отсюда правило для проб: проба обязана
# ПОВТОРЯТЬ использование. Проверка «есть ли заголовок» отдельно от прагмы
# отвечает не на тот вопрос, и один раз уже ответила неверно (см. HAVE__XGETBV
# ниже).

set(sodium_dir "${CMAKE_CURRENT_SOURCE_DIR}/upstream")
set(sodium_src "${sodium_dir}/src/libsodium")

include(CheckCSourceCompiles)
include(CheckIncludeFile)
include(CheckSymbolExists)

# --- версия -----------------------------------------------------------------
#
# Достаём из configure.ac, а не вписываем руками: вписанная руками разъезжается
# с деревом на первом же обновлении, и окно «О программе» врёт с уверенным
# видом. configure.ac для того и оставлен в upstream/.

file(READ "${sodium_dir}/configure.ac" sodium_ac)
string(REGEX MATCH "AC_INIT\\(\\[libsodium\\],\\[([0-9]+\\.[0-9]+\\.[0-9]+)\\]" _m "${sodium_ac}")
set(SODIUM_VENDORED_VERSION "${CMAKE_MATCH_1}")
string(REGEX MATCH "SODIUM_LIBRARY_VERSION_MAJOR=([0-9]+)" _m "${sodium_ac}")
set(SODIUM_LIBRARY_VERSION_MAJOR "${CMAKE_MATCH_1}")
string(REGEX MATCH "SODIUM_LIBRARY_VERSION_MINOR=([0-9]+)" _m "${sodium_ac}")
set(SODIUM_LIBRARY_VERSION_MINOR "${CMAKE_MATCH_1}")

if(NOT SODIUM_VENDORED_VERSION OR NOT SODIUM_LIBRARY_VERSION_MAJOR)
    message(FATAL_ERROR
        "libsodium: не удалось прочитать версию из upstream/configure.ac.\n"
        "  Похоже, обновление сменило формат — поправьте регулярку здесь.")
endif()

set(SODIUM_VENDORED_VERSION "${SODIUM_VENDORED_VERSION}" PARENT_SCOPE)

# version.h чужой скрипт собирает из version.h.in; делаем то же сами.
set(VERSION "${SODIUM_VENDORED_VERSION}")
set(SODIUM_LIBRARY_MINIMAL_DEF "")
configure_file("${sodium_src}/include/sodium/version.h.in"
               "${CMAKE_CURRENT_BINARY_DIR}/include/sodium/version.h" @ONLY)

set(CMAKE_REQUIRED_DEFINITIONS -D_GNU_SOURCE)
set(CMAKE_REQUIRED_QUIET ON)

set(sodium_is_x86 OFF)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|i[3-6]86)$")
    set(sodium_is_x86 ON)
endif()

# --- пробы: расширения процессора -------------------------------------------
#
# Проба повторяет исходник: прагма, затем заголовок, затем настоящий вызов.
# Без вызова компилятор принимает заголовок и спотыкается уже на теле.

function(sodium_check_isa var targets header snippet)
    check_c_source_compiles("
#pragma GCC target(\"${targets}\")
#include <${header}>
int main(void) { ${snippet}; return 0; }
" ${var})
    set(${var} "${${var}}" PARENT_SCOPE)
endfunction()

if(sodium_is_x86)
    sodium_check_isa(HAVE_MMINTRIN_H      "mmx"        mmintrin.h  "__m64 x = _mm_setzero_si64(); (void) x")
    sodium_check_isa(HAVE_EMMINTRIN_H     "sse2"       emmintrin.h "__m128i x = _mm_setzero_si128(); (void) x")
    sodium_check_isa(HAVE_PMMINTRIN_H     "sse3"       pmmintrin.h "__m128d x = _mm_setzero_pd(); x = _mm_hadd_pd(x, x); (void) x")
    sodium_check_isa(HAVE_TMMINTRIN_H     "ssse3"      tmmintrin.h "__m128i x = _mm_setzero_si128(); x = _mm_abs_epi8(x); (void) x")
    sodium_check_isa(HAVE_SMMINTRIN_H     "sse4.1"     smmintrin.h "__m128i x = _mm_setzero_si128(); x = _mm_min_epu32(x, x); (void) x")
    sodium_check_isa(HAVE_AVXINTRIN_H     "avx"        immintrin.h "__m256d x = _mm256_setzero_pd(); x = _mm256_add_pd(x, x); (void) x")
    sodium_check_isa(HAVE_AVX2INTRIN_H    "avx2"       immintrin.h "__m256i x = _mm256_setzero_si256(); x = _mm256_add_epi32(x, x); (void) x")
    sodium_check_isa(HAVE_AVX512FINTRIN_H "avx512f"    immintrin.h "__m512i x = _mm512_setzero_si512(); x = _mm512_add_epi32(x, x); (void) x")
    sodium_check_isa(HAVE_WMMINTRIN_H     "aes,pclmul" wmmintrin.h "__m128i x = _mm_setzero_si128(); x = _mm_aesenc_si128(x, x); (void) x")
    sodium_check_isa(HAVE_RDRAND          "rdrnd"      immintrin.h "unsigned int v; (void) _rdrand32_step(&v)")

    check_c_source_compiles("
#include <cpuid.h>
int main(void) { unsigned int a, b, c, d; __cpuid_count(0, 0, a, b, c, d); (void) a; return 0; }
" HAVE_CPUID)

    # HAVE__XGETBV — про ВСТРОЕННУЮ ФУНКЦИЮ _xgetbv, и спрашивается она в
    # runtime.c БЕЗ всякой прагмы и без immintrin.h. Первая редакция этой
    # пробы проверяла вместо неё голый asm-байткод, получала «да» — и сборка
    # падала на implicit declaration. Проба обязана повторять использование.
    check_c_source_compiles("
#include <stdint.h>
int main(void) { return (int) (uint32_t) _xgetbv(0); }
" HAVE__XGETBV)

    # Пробы asm — дословно из configure.ac (строки 709 и 738), включая отказ
    # для Windows x86_64: там другое соглашение о вызовах.
    check_c_source_compiles("
int main(void) {
#if defined(__amd64) || defined(__amd64__) || defined(__x86_64__)
# if defined(__CYGWIN__) || defined(__MINGW32__) || defined(__MINGW64__) || defined(_WIN32) || defined(_WIN64)
#  error Windows x86_64 calling conventions are not supported yet
# endif
#else
# error !x86_64
#endif
    unsigned char i = 0, o = 0, t;
    __asm__ __volatile__ (\"pxor %%xmm12, %%xmm6 \\n\"
                          \"movb (%[i]), %[t] \\n\"
                          \"addb %[t], (%[o]) \\n\"
                          : [t] \"=&r\"(t)
                          : [o] \"D\"(&o), [i] \"S\"(&i)
                          : \"memory\", \"flags\", \"cc\");
    return 0;
}
" HAVE_AMD64_ASM)

    check_c_source_compiles("
int main(void) {
#if defined(__amd64) || defined(__amd64__) || defined(__x86_64__)
# if defined(__CYGWIN__) || defined(__MINGW32__) || defined(__MINGW64__) || defined(_WIN32) || defined(_WIN64)
#  error Windows x86_64 calling conventions are not supported yet
# endif
#else
# error !x86_64
#endif
    __asm__ __volatile__ (\"vpunpcklqdq %xmm0,%xmm13,%xmm0\");
    return 0;
}
" HAVE_AVX_ASM)
endif()

# ЗДЕСЬ ЖИВЁТ ТИХИЙ ПРОВАЛ, И ОН СТЕРЕЖЁТСЯ ЯВНО.
#
# runtime.c читает регистр XCR0, чтобы узнать, разрешил ли AVX сам ОС. Прочесть
# его он умеет ровно двумя способами: встроенной _xgetbv (её у gcc нет) либо
# asm-вставкой под HAVE_AVX_ASM. Если не объявлено НИ ОДНОГО, переменная xcr0
# остаётся нулём, has_avx становится нулём — и все пути AVX и AVX2 выключаются
# НА ИСПОЛНЕНИИ. Сборка при этом проходит, наборы проходят, всё «работает» —
# просто втрое медленнее и молча. Поэтому: не догадка, а проверка.
if(sodium_is_x86 AND HAVE_AVX2INTRIN_H AND NOT HAVE_AVX_ASM AND NOT HAVE__XGETBV)
    message(WARNING
        "libsodium: компилятор умеет AVX2, но прочитать XCR0 нечем "
        "(нет ни _xgetbv, ни asm-вставки). AVX и AVX2 будут отключены НА "
        "ИСПОЛНЕНИИ, и крипто пойдёт по портируемому пути. Разберитесь, "
        "почему провалились пробы HAVE_AVX_ASM/HAVE__XGETBV.")
endif()

# --- пробы: заголовки и функции ---------------------------------------------

foreach(pair "sys/mman.h;HAVE_SYS_MMAN_H" "sys/param.h;HAVE_SYS_PARAM_H"
             "sys/random.h;HAVE_SYS_RANDOM_H" "sys/auxv.h;HAVE_SYS_AUXV_H"
             "alloca.h;HAVE_ALLOCA_H" "intrin.h;HAVE_INTRIN_H" "cet.h;HAVE_CET_H")
    list(GET pair 0 hdr)
    list(GET pair 1 var)
    check_include_file("${hdr}" ${var})
endforeach()

check_symbol_exists(mmap             "sys/mman.h" HAVE_MMAP)
check_symbol_exists(mlock            "sys/mman.h" HAVE_MLOCK)
check_symbol_exists(madvise          "sys/mman.h" HAVE_MADVISE)
check_symbol_exists(mprotect         "sys/mman.h" HAVE_MPROTECT)
check_symbol_exists(posix_memalign   "stdlib.h"   HAVE_POSIX_MEMALIGN)
check_symbol_exists(getpid           "unistd.h"   HAVE_GETPID)
check_symbol_exists(sysconf          "unistd.h"   HAVE_SYSCONF)
check_symbol_exists(nanosleep        "time.h"     HAVE_NANOSLEEP)
check_symbol_exists(raise            "signal.h"   HAVE_RAISE)
check_symbol_exists(explicit_bzero   "string.h"   HAVE_EXPLICIT_BZERO)
check_symbol_exists(explicit_memset  "string.h"   HAVE_EXPLICIT_MEMSET)
check_symbol_exists(memset_s         "string.h"   HAVE_MEMSET_S)
check_symbol_exists(memset_explicit  "string.h"   HAVE_MEMSET_EXPLICIT)
check_symbol_exists(getentropy       "unistd.h;sys/random.h" HAVE_GETENTROPY)
check_symbol_exists(getauxval        "sys/auxv.h" HAVE_GETAUXVAL)
check_symbol_exists(elf_aux_info     "sys/auxv.h" HAVE_ELF_AUX_INFO)

# getrandom проверяем ВЫЗОВОМ, а не наличием символа: HAVE_LINUX_COMPATIBLE_GETRANDOM
# — это именно линуксовая семантика (флаги, поведение при нехватке энтропии),
# и на других системах символ бывает, а семантика другая.
check_c_source_compiles("
#include <sys/random.h>
int main(void) { unsigned char b[1]; return (int) getrandom(b, 1U, 0U); }
" HAVE_LINUX_COMPATIBLE_GETRANDOM)
if(HAVE_LINUX_COMPATIBLE_GETRANDOM)
    set(HAVE_GETRANDOM 1)
endif()

# --- пробы: свойства компилятора --------------------------------------------

check_c_source_compiles("int main(void) { __int128 x = 0; (void) x; return 0; }" HAVE_TI_MODE)
check_c_source_compiles("int main(void) { int n = 4; char a[n]; a[0] = 0; return a[0]; }" HAVE_C_VARARRAYS)
check_c_source_compiles("
__attribute__((weak)) void wf(void) {}
int main(void) { wf(); return 0; }
" HAVE_WEAK_SYMBOLS)
check_c_source_compiles("
int main(void) { static volatile int n = 0; __sync_fetch_and_add(&n, 1); return n - 1; }
" HAVE_ATOMIC_OPS)
check_c_source_compiles("
#include <stdatomic.h>
int main(void) { atomic_thread_fence(memory_order_seq_cst); return 0; }
" HAVE_C11_MEMORY_FENCES)
check_c_source_compiles("int main(void) { __sync_synchronize(); return 0; }" HAVE_GCC_MEMORY_FENCES)
check_c_source_compiles("
int main(void) { int x = 0; __asm__ __volatile__ (\"\" : \"+r\"(x)); return x; }
" HAVE_INLINE_ASM)

if(HAVE_ALLOCA_H)
    set(HAVE_ALLOCA 1)
endif()
if(HAVE_MPROTECT)
    set(HAVE_PAGE_PROTECTION 1)
endif()

find_package(Threads)
if(CMAKE_USE_PTHREADS_INIT)
    set(HAVE_PTHREAD 1)
endif()

# --- источники --------------------------------------------------------------
#
# GLOB, а не список из 119 имён, И ЭТО НАРОЧНО. Дерево upstream/ мы не правим:
# update.sh заменяет его целиком. Записанный список расходился бы с деревом
# молча (файл добавили — он не собрался), а GLOB не может: что лежит, то и
# собирается. Все файлы с расширениями процессора самозащищены макросами
# HAVE_*, так что лишнего в объектники не попадёт.
#
# .S ДОБАВЛЯЮТСЯ ОТДЕЛЬНО И ПОШТУЧНО. В sandy2x настоящий исходник ровно один
# — sandy2x.S; остальные .S он ВКЛЮЧАЕТ в себя (потому и объявляет IN_SANDY2X
# сам). Отдать их GLOB'у значило бы собрать каждый ещё и отдельно и получить
# дубли символов. Оба .S самозащищены: без HAVE_AMD64_ASM / HAVE_AVX_ASM из
# них выходит пустой объектник.

file(GLOB_RECURSE sodium_sources CONFIGURE_DEPENDS "${sodium_src}/*.c")
list(LENGTH sodium_sources sodium_count)
if(sodium_count LESS 100)
    message(FATAL_ERROR "libsodium: найдено всего ${sodium_count} исходников — дерево upstream/ неполное?")
endif()

if(HAVE_AMD64_ASM OR HAVE_AVX_ASM)
    enable_language(ASM)
    if(HAVE_AMD64_ASM)
        list(APPEND sodium_sources "${sodium_src}/crypto_stream/salsa20/xmm6/salsa20_xmm6-asm.S")
    endif()
    if(HAVE_AVX_ASM)
        list(APPEND sodium_sources "${sodium_src}/crypto_scalarmult/curve25519/sandy2x/sandy2x.S")
    endif()
endif()

add_library(libsodium STATIC ${sodium_sources})
add_library(Sodium::Sodium ALIAS libsodium)

# Каталог include/sodium открыт НАРУЖУ, и это вынужденно. Публичный
# <sodium.h> включает "sodium/version.h", а тот — "export.h" БЕЗ префикса,
# рассчитывая лежать с ним в одном каталоге. У upstream так и выходит:
# configure генерирует version.h прямо в дерево исходников, рядом с export.h
# (и libsodium-cmake повторяет это же). Мы в вендоренное дерево не пишем — оно
# лежит в git и заменяется целиком через update.sh, — поэтому version.h
# рождается в каталоге сборки, а export.h ему находится через этот путь.
# Цена: потребителю становятся видны внутренние заголовки libsodium. Она
# невелика — включение в кавычках сперва смотрит в каталог самого файла, так
# что своя "utils.h" у потребителя чужую не поймает.
target_include_directories(libsodium
    PUBLIC  "${sodium_src}/include"
            "${sodium_src}/include/sodium"
            "${CMAKE_CURRENT_BINARY_DIR}/include"
            "${CMAKE_CURRENT_BINARY_DIR}/include/sodium"
    PRIVATE "${sodium_src}")

# SODIUM_STATIC делает SODIUM_EXPORT пустым (см. include/sodium/export.h) —
# PUBLIC, потому что его должен видеть и тот, кто включает <sodium.h>.
# CONFIGURED=1 гасит предупреждение «библиотека собирается недокументированным
# способом»: способ у нас как раз документирован — этим файлом.
target_compile_definitions(libsodium
    PUBLIC  SODIUM_STATIC
    PRIVATE CONFIGURED=1 _GNU_SOURCE)

foreach(have HAVE_MMINTRIN_H HAVE_EMMINTRIN_H HAVE_PMMINTRIN_H HAVE_TMMINTRIN_H
             HAVE_SMMINTRIN_H HAVE_AVXINTRIN_H HAVE_AVX2INTRIN_H
             HAVE_AVX512FINTRIN_H HAVE_WMMINTRIN_H HAVE_RDRAND HAVE_CPUID
             HAVE__XGETBV HAVE_AMD64_ASM HAVE_AVX_ASM
             HAVE_SYS_MMAN_H HAVE_SYS_PARAM_H HAVE_SYS_RANDOM_H
             HAVE_SYS_AUXV_H HAVE_ALLOCA_H HAVE_ALLOCA HAVE_INTRIN_H HAVE_CET_H
             HAVE_MMAP HAVE_MLOCK HAVE_MADVISE HAVE_MPROTECT HAVE_PAGE_PROTECTION
             HAVE_POSIX_MEMALIGN HAVE_GETPID HAVE_SYSCONF HAVE_NANOSLEEP
             HAVE_RAISE HAVE_EXPLICIT_BZERO HAVE_EXPLICIT_MEMSET HAVE_MEMSET_S
             HAVE_MEMSET_EXPLICIT HAVE_GETENTROPY HAVE_GETAUXVAL
             HAVE_ELF_AUX_INFO HAVE_GETRANDOM HAVE_LINUX_COMPATIBLE_GETRANDOM
             HAVE_TI_MODE HAVE_C_VARARRAYS HAVE_WEAK_SYMBOLS HAVE_ATOMIC_OPS
             HAVE_C11_MEMORY_FENCES HAVE_GCC_MEMORY_FENCES HAVE_INLINE_ASM
             HAVE_PTHREAD)
    if(${have})
        target_compile_definitions(libsodium PRIVATE ${have}=1)
    endif()
endforeach()

if(HAVE_PTHREAD)
    target_link_libraries(libsodium PUBLIC Threads::Threads)
endif()

# Порядок байт. Проверяем, а не предполагаем: NATIVE_LITTLE_ENDIAN включает в
# libsodium быстрые пути, и ошибиться здесь значит молча испортить данные.
# CMAKE_C_BYTE_ORDER есть с CMake 3.20 — ровно наш минимум; модуль
# TestBigEndian с тех пор объявлен устаревшим.
if(NOT CMAKE_C_BYTE_ORDER STREQUAL "BIG_ENDIAN")
    target_compile_definitions(libsodium PRIVATE NATIVE_LITTLE_ENDIAN=1)
endif()

# Чужое — всегда -O3, и в отладочной сборке тоже (правило владельца, см.
# корневой CMakeLists): крипто мы не отлаживаем.
if(NOT MSVC)
    target_compile_options(libsodium PRIVATE $<$<COMPILE_LANGUAGE:C>:-O3>)
endif()
