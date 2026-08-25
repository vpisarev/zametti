# libheif 1.23.2, вендоринг чужим CMake. Взято из
# https://github.com/strukturag/libheif, тег v1.23.2; чем именно обрезано
# дерево — см. update.sh рядом.
#
# ЗАЧЕМ ОН НАМ. Это разбор контейнера ISOBMFF, в котором лежат HEIC и AVIF:
# сетки плиток, повороты, альфа отдельным item'ом, ICC и nclx. Сама она не
# декодирует ничего — кодеки зовёт со стороны.
#
# КТО ЕЙ ДЕКОДИРУЕТ. HEVC (то есть HEIC) — вендоренная libde265, вписана
# built-in. AV1 (то есть AVIF) — НАШ СОБСТВЕННЫЙ плагин на libgav1: libheif
# про libgav1 не знает вовсе, в её libheif/plugins/ лежат только aom, dav1d,
# ffmpeg и webcodecs. Регистрируется он публичным
# heif_register_decoder_plugin(), патча к чужому дереву нет и не будет.
# Поэтому здесь ВСЕ кодеки AV1 выключены, и до регистрации нашего плагина
# библиотека про AVIF честно не умеет ничего.
#
# ТОЛЬКО ЧТЕНИЕ. Ни одного энкодера: пишем мы один формат, JPEG XL.

set(heif_dir "${CMAKE_CURRENT_SOURCE_DIR}/upstream")

# Порядок add_subdirectory в корне обязан быть таким: libde265 → libgav1 →
# libheif. Ловим это здесь и внятно, а не на «Could NOT find LIBDE265»
# двадцатью строками ниже.
if(NOT TARGET de265)
    message(FATAL_ERROR
        "libheif: цель de265 ещё не заведена.\n"
        "  add_subdirectory(3rdparty/libde265) обязан идти РАНЬШЕ "
        "add_subdirectory(3rdparty/libheif) в корневом CMakeLists.txt.")
endif()

# Версия — из чужого project(), а не вписанная руками.
file(READ "${heif_dir}/CMakeLists.txt" heif_root_cmake)
string(REGEX MATCH "project *\\(libheif[^)]*VERSION ([0-9]+\\.[0-9]+\\.[0-9]+)"
       _m "${heif_root_cmake}")
set(HEIF_VENDORED_VERSION "${CMAKE_MATCH_1}")
if(NOT HEIF_VENDORED_VERSION)
    message(FATAL_ERROR
        "libheif: не удалось прочитать версию из upstream/CMakeLists.txt.\n"
        "  Похоже, обновление сменило формат project() — поправьте регулярку здесь.")
endif()
set(HEIF_VENDORED_VERSION "${HEIF_VENDORED_VERSION}" PARENT_SCOPE)

# --- ПОДСОВЫВАЕМ ВЕНДОРЕННУЮ libde265 ВМЕСТО СИСТЕМНОЙ ---------------------
#
# Чужой cmake/modules/FindLIBDE265.cmake делает find_path и find_library, а обе
# команды НЕ ТРОГАЮТ переменную, если она уже лежит в кэше. Значит поиска не
# будет вовсе — а это ровно то, чего требует наш toolchain переносимой сборки
# (packaging/linux/toolchains/linux-zsys.cmake держит CMAKE_FIND_ROOT_PATH_MODE_*
# в ONLY: любой find_* мимо sysroot запрещён нарочно).
#
# В LIBDE265_LIBRARY кладётся ИМЯ ЦЕЛИ, а не путь к файлу. Так честнее: из
# имени цели приезжают и порядок сборки, и её INTERFACE-каталоги, и
# LIBDE265_STATIC_BUILD. Чужой libfind_process сверяет значение только со
# строкой «-NOTFOUND», а find_package_handle_standard_args — только на
# непустоту; проверено запуском на CMake 4.2.
#
# LIBDE265_INCLUDE_DIR указывает на КАТАЛОГ СБОРКИ libde265, и это не описка:
# оттуда чужой модуль читает номер версии из libde265/de265-version.h, который
# ПОРОЖДАЕТСЯ configure_file.
if(NOT EXISTS "${DE265_GENERATED_INCLUDE_DIR}/libde265/de265-version.h")
    message(FATAL_ERROR
        "libheif: нет ${DE265_GENERATED_INCLUDE_DIR}/libde265/de265-version.h.\n"
        "  Его порождает configure_file() в libde265, и из него чужой "
        "FindLIBDE265 читает версию.\n"
        "  Проверьте порядок add_subdirectory в корневом CMakeLists.txt.")
endif()
set(LIBDE265_INCLUDE_DIR "${DE265_GENERATED_INCLUDE_DIR}" CACHE PATH   "вендоренная libde265" FORCE)
set(LIBDE265_LIBRARY     "de265"                          CACHE STRING "ИМЯ ЦЕЛИ, не путь"    FORCE)

# --- КОДЕКИ ----------------------------------------------------------------
#
# Из всего списка включён ровно один — libde265, и он built-in, а не плагин.
#
# Часть этих ключей и так OFF по умолчанию. Ставим их ЯВНО: option() не
# притрагивается к уже заданной переменной, а кэш переживает смену наших
# правил. Тот же довод, что в комментарии 3rdparty/libwebp/CMakeLists.txt.
set(WITH_LIBDE265        ON  CACHE BOOL "" FORCE)
set(WITH_LIBDE265_PLUGIN OFF CACHE BOOL "" FORCE)
foreach(heif_codec X265 KVAZAAR UVG266 VVDEC VVENC X264 OpenH264_DECODER
                   DAV1D AOM_DECODER AOM_ENCODER SvtEnc RAV1E
                   JPEG_DECODER JPEG_ENCODER OpenJPEG_ENCODER OpenJPEG_DECODER
                   FFMPEG_DECODER OPENJPH_ENCODER)
    set(WITH_${heif_codec}        OFF CACHE BOOL "" FORCE)
    set(WITH_${heif_codec}_PLUGIN OFF CACHE BOOL "" FORCE)
endforeach()
set(WITH_WEBCODECS          OFF CACHE BOOL "" FORCE)
set(WITH_UNCOMPRESSED_CODEC OFF CACHE BOOL "" FORCE)

# --- ОБВЕС -----------------------------------------------------------------
#
# ENABLE_PLUGIN_LOADING гасит dlopen-реестр кодеков, зависимость от
# ${CMAKE_DL_LIBS} и install(DIRECTORY) в /usr/lib/libheif. Программа, вшившая
# libheif статически, не должна подхватывать чужие .so с машины пользователя.
# Наш способ регистрации от этого не страдает: heif_register_decoder_plugin
# лежит в plugin_registry.cc ВНЕ всяких #if.
set(ENABLE_PLUGIN_LOADING OFF CACHE BOOL "" FORCE)

# libsharpyuv — это RGB→YUV, то есть НАПРАВЛЕНИЕ ЗАПИСИ (rgb2yuv_sharp.cc и
# выбор умолчания для понижения цветности). Энкодеров у нас нет ни одного.
# Наша вендоренная цель sharpyuv из 3rdparty/libwebp тут не пригодилась бы —
# это был бы мёртвый код и лишняя связь между каталогами.
set(WITH_LIBSHARPYUV          OFF CACHE BOOL "" FORCE)
set(WITH_LIBSHARPYUV_INTERNAL OFF CACHE BOOL "" FORCE)

# WITH_HEADER_COMPRESSION — цена названа честно: это не только запись, но и
# ЧТЕНИЕ файлов со сжатыми item-property боксами. Гасим потому, что с ON чужой
# CMake делает find_package(ZLIB) и find_package(Brotli), то есть тащит zlib
# мимо нашей вендоренной 3rdparty/zlib и brotli мимо той, что лежит внутри
# 3rdparty/libjxl. Две zlib в одном двоичном файле — дорогая плата за формат,
# которого в природе почти нет. У upstream этот ключ тоже OFF по умолчанию.
# Прилетит такой файл — включим и подсунем цели, как сделано с libde265.
set(WITH_HEADER_COMPRESSION OFF CACHE BOOL "" FORCE)

# BUILD_DOCUMENTATION при ON делает find_package(Doxygen) и заводит
# add_custom_target(doc_doxygen ALL) — то есть doxygen побежал бы при КАЖДОЙ
# нашей сборке. Sysroot тут не защита: CMAKE_FIND_ROOT_PATH_MODE_PROGRAM в
# нашем toolchain стоит NEVER нарочно, хостовые программы искать разрешено.
set(BUILD_DOCUMENTATION OFF CACHE BOOL "" FORCE)

# gdk-pixbuf зовёт pkg_check_modules на хост.
set(WITH_GDK_PIXBUF OFF CACHE BOOL "" FORCE)

set(WITH_EXAMPLES           OFF CACHE BOOL "" FORCE)
set(WITH_EXAMPLE_HEIF_THUMB OFF CACHE BOOL "" FORCE)
set(WITH_EXAMPLE_HEIF_VIEW  OFF CACHE BOOL "" FORCE)
set(BUILD_DEVELOPMENT_TOOLS OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING           OFF CACHE BOOL "" FORCE)
set(WITH_FUZZERS            OFF CACHE BOOL "" FORCE)
set(ENABLE_COVERAGE         OFF CACHE BOOL "" FORCE)
set(BUILD_FRAMEWORK         OFF CACHE BOOL "" FORCE)

# Многопоточность плиток оставляем: это скорость чтения плиточных HEIC, а
# find_package(Threads) идёт пробами компилятора, а не через sysroot.
set(ENABLE_MULTITHREADING_SUPPORT ON CACHE BOOL "" FORCE)
set(ENABLE_PARALLEL_TILE_DECODING ON CACHE BOOL "" FORCE)
set(WITH_REDUCED_VISIBILITY       ON CACHE BOOL "" FORCE)

# --- ZLIB: НАША, НО МИМО ЧУЖОГО EXPORT-НАБОРА ------------------------------
#
# 3rdparty/zlib заранее заполняет кэш (ZLIB_FOUND, ZLIB_INCLUDE_DIR, …) и
# заводит псевдоним ZLIB::ZLIB — это сделано ради libtiff. libheif видит
# ZLIB_FOUND истинным и линкует ZLIB::ZLIB сама, а её install(EXPORT
# libheif-config) на этом падает: «target heif requires target zlib that is not
# in any export set».
#
# Отказаться от zlib нельзя: HAVE_ZLIB у libheif — это ЧТЕНИЕ item'ов с
# кодировкой deflate/compress_zlib и сжатых EXIF/XMP. Системная libheif 1.21.2,
# с которой мы сверяемся, собрана с zlib; без него мы разошлись бы с судьёй на
# ровном месте.
#
# Поэтому: от ЧУЖОГО CMake zlib прячем (обычная переменная перекрывает кэш в
# этой области видимости и ниже), а нужное доделываем сами — заголовки и
# HAVE_ZLIB прямо цели heif, а сам архив приходит через нашу INTERFACE-цель,
# которую никто никуда не экспортирует.
set(ZLIB_FOUND FALSE)

add_subdirectory(upstream EXCLUDE_FROM_ALL)

target_compile_definitions(heif PRIVATE HAVE_ZLIB=1)
target_include_directories(heif PRIVATE
                           $<TARGET_PROPERTY:zlib,INTERFACE_INCLUDE_DIRECTORIES>)

# Одна цель наружу.
#
# ДВА КАТАЛОГА, И ВТОРОЙ НЕ ОЧЕВИДЕН. У цели heif собственные PUBLIC-каталоги
# указывают на ${PROJECT_SOURCE_DIR}/include, КОТОРОГО В ДЕРЕВЕ 1.23.2 НЕТ
# ВОВСЕ, — то есть «прилинковать heif и включить <libheif/heif.h>» само по себе
# не работает. Первый каталог даёт <libheif/heif.h>, второй —
# <libheif/heif_version.h>, который порождается configure_file в каталоге
# сборки, в upstream/libheif/heif_version.h. Каталог нужен НА УРОВЕНЬ ВЫШЕ
# файла: включают его как <libheif/heif_version.h>.
#
# Первая редакция ошиблась здесь на один уровень, и на этой машине ошибка НЕ
# ПРОЯВИЛАСЬ: у gcc в неявных путях стоит /usr/include/x86_64-linux-gnu, где
# лежит heif_version.h ОТ СИСТЕМНОЙ libheif 1.21.2. То есть заголовки брались
# из нашего дерева, а номер версии — из чужой библиотеки другой версии.
# Вскрылось только сборкой против sysroot, где системной libheif нет.
add_library(libheif INTERFACE)
target_link_libraries(libheif INTERFACE heif zlib)
target_include_directories(libheif SYSTEM INTERFACE
                           "${heif_dir}/libheif/api"
                           "${CMAKE_CURRENT_BINARY_DIR}/upstream")

# Чужой код чужими правилами: zametti-code-rules ему не цепляется.
