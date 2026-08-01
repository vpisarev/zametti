# libjxl 0.12.0, вендоринг подмножеством дерева. Взято из
# https://github.com/libjxl/libjxl, тег v0.12.0 (1 июля 2026); что именно
# копируется и что выбрасывается — в update.sh рядом, там же команда обновления.
#
# Почему вендоринг, а не системная библиотека. Двух доводов хватило:
#
#   * SSIMULACRA2 — арбитр лестницы качества — живёт в tools/ и питается
#     ВНУТРЕННИМИ заголовками (lib/jxl/image.h, enc_xyb.h). Пакет libjxl-dev их
#     не ставит вовсе, то есть системная библиотека арбитра не даёт в принципе;
#   * на машине владельца стоит libjxl0.11 без заголовков, а нам нужен 0.12 —
#     сборка на чистой машине зависела бы от набора пакетов дистрибутива.
#
# Почему одна зависимость закрывает три задачи: кодек JXL, БАЙТ-ТОЧНЫЙ транскод
# JPEG→JXL (реконструкция исходного файла до последнего байта — на этом стоит
# инвариант A этапа 8) и метрику SSIMULACRA2.
#
# Почему JPEG XL основным форматом хранения — решение владельца, «дизайним на
# века»: картинки живут внутри программы, экспорт всегда сконвертирует, а
# битовая глубина и цветовое пространство исходника сохраняются. Вариант
# AVIF+WebP отвергнут: webp-lossless умеет только 8 бит на канал.
#
# Цена (замеры, Release, -j8): дерево 17 МБ и 1226 файлов; холодная сборка
# 57.9 с, ноль предупреждений; libjxl.a 6.8 МБ. Наружу не тянет ничего, кроме
# libstdc++/libm/libc/pthread — проверено линковкой пробника: ни png, ни zlib,
# ни lcms не нужны, они питают только lib/extras, которых мы не берём
# (декодирует входные форматы Qt).
#
# Что выброшено из дерева, проверено сверкой: собранные библиотеки (libjxl.a,
# libjxl_cms.a, libhwy.a, libjxl_gauss_blur.a) ПОБАЙТОВО совпадают с теми, что
# даёт полное дерево со всеми подмодулями.

set(JXL_VENDORED_VERSION "0.12.0" PARENT_SCOPE)

# Настройки чужой сборки задаём через кэш и с FORCE: у libjxl это не option(), а
# set(... CACHE), и обычная переменная тут не сработала бы. Имена свои, чужого
# не задеваем.
macro(zametti_jxl_option name value)
    set(${name} ${value} CACHE BOOL "выставлено zametti" FORCE)
endmacro()

# Что нам нужно от libjxl.
zametti_jxl_option(JPEGXL_ENABLE_TRANSCODE_JPEG ON)   # инвариант A: байт-точная реконструкция
zametti_jxl_option(JPEGXL_ENABLE_BOXES          ON)   # боксы контейнера: EXIF, XMP
zametti_jxl_option(JPEGXL_ENABLE_SKCMS          ON)   # цвет через skcms: вчетверо легче lcms

# Что не нужно. Каждая строка — это ещё и подмодуль, который не пришлось брать.
zametti_jxl_option(JPEGXL_ENABLE_TOOLS      OFF)
zametti_jxl_option(JPEGXL_ENABLE_BENCHMARK  OFF)
zametti_jxl_option(JPEGXL_ENABLE_EXAMPLES   OFF)
zametti_jxl_option(JPEGXL_ENABLE_MANPAGES   OFF)
zametti_jxl_option(JPEGXL_ENABLE_DOXYGEN    OFF)
zametti_jxl_option(JPEGXL_ENABLE_JNI        OFF)
zametti_jxl_option(JPEGXL_ENABLE_SJPEG      OFF)
zametti_jxl_option(JPEGXL_ENABLE_OPENEXR    OFF)
zametti_jxl_option(JPEGXL_ENABLE_PLUGINS    OFF)
zametti_jxl_option(JPEGXL_ENABLE_DEVTOOLS   OFF)
zametti_jxl_option(JPEGXL_ENABLE_VIEWERS    OFF)
zametti_jxl_option(JPEGXL_ENABLE_FUZZERS    OFF)
zametti_jxl_option(JPEGXL_BUNDLE_LIBPNG     OFF)

# Тесты libjxl требуют googletest, которого мы не берём. Гасим до
# add_subdirectory: наши собственные наборы на BUILD_TESTING не завязаны (они
# висят на enable_testing() в корне), так что это никого не задевает.
set(BUILD_TESTING OFF CACHE BOOL "выставлено zametti: тесты libjxl не собираем" FORCE)

# Версия в заголовке утилит libjxl берётся из git; вендоренному дереву взять её
# неоткуда, и без этого в сборке светилось бы «(unknown)». К версии самой
# библиотеки отношения не имеет — та зашита в lib/CMakeLists.txt.
set(JPEGXL_VERSION "0.12.0" CACHE STRING "выставлено zametti" FORCE)

# EXCLUDE_FROM_ALL: собирается только то, за что кто-то потянул. Установочных
# целей libjxl нам не надо, а «собрать всё» не должно тащить их обвес.
add_subdirectory(upstream EXCLUDE_FROM_ALL)

# --- арбитр лестницы ------------------------------------------------------
#
# SSIMULACRA2 своей цели у libjxl нет (она заводится только вместе с tools),
# поэтому собираем два файла сами. jxl-internal — те же объекты, что и
# публичный jxl, но с внутренними заголовками наружу; линковать надо ИМЕННО
# его, иначе символы задвоятся.

add_library(jxl_ssimulacra2 STATIC
    upstream/tools/ssimulacra2.cc
    upstream/tools/no_memory_manager.cc
)
target_include_directories(jxl_ssimulacra2 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/upstream)
target_link_libraries(jxl_ssimulacra2 PUBLIC jxl-internal jxl_gauss_blur jxl_cms)
set_target_properties(jxl_ssimulacra2 PROPERTIES POSITION_INDEPENDENT_CODE ON)

# --- то, что видит наш код ------------------------------------------------
#
# Одна цель на всё: кодек, потоки и арбитр. Отдельно разбираться, что куда
# линкуется, потребителю незачем.

add_library(libjxl INTERFACE)
target_link_libraries(libjxl INTERFACE jxl-internal jxl_threads jxl_cms jxl_ssimulacra2)

# Чужой код чужими правилами: zametti-warnings (-Wall -Wextra -Wpedantic и, по
# ключу, -Werror) сюда НЕ цепляется. Иначе каждое обновление libjxl ломало бы
# нам сборку своими предупреждениями. Тот же уговор, что с md4c, blake3 и zstd.
