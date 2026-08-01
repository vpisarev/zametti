# libtiff 4.7.0, вендоринг подмножеством дерева. Взято из
# https://gitlab.com/libtiff/libtiff, тег v4.7.0; что копируется — в update.sh
# рядом.
#
# ЗАЧЕМ. Qt читает TIFF, но не всякий и не всё:
#
#   * 16-битный RGB с профилем он читает ВЕРНО — проверено сверкой на файле из
#     потока DxO: полная глубина, профиль Display P3, значения сходятся с
#     независимым разбором с точностью до единицы;
#   * а CIELab он ТИХО ПЕРЕВИРАЕТ. На синтетике нейтральный серый L=50 выходит
#     (133,123,107) вместо (119,119,119), белый — кремовым. На файле Эрмитажа
#     средняя ошибка 15.6 по красному и 17.9 по синему.
#
# Корень найден: плагин Qt зовёт у libtiff путь TIFFReadRGBAImage, а тот
# применяет СВОЮ модель дисплея (гамма 2.2, своя опорная белая), а не
# колориметрический перевод. Утилита tiff2rgba даёт ту же ошибку в ту же
# сторону, так что эталоном она тоже не является.
#
# ОТСЮДА ГЛАВНОЕ ПРАВИЛО РАБОТЫ С НЕЙ:
#
#     брать СЫРЫЕ отсчёты (TIFFReadScanline) и считать цвет самим;
#     TIFFReadRGBAImage не звать никогда.
#
# Проверено: наша математика Lab→sRGB с белой точкой D50 совпадает с
# независимым разбором до единицы, тогда как tiff2rgba расходится на одиннадцать.
#
# Что libtiff даёт сверх Qt, на файлах владельца: цветовое пространство,
# глубину и число каналов, сжатие, ориентацию, ICC (у CMYK-файла профиль в
# 424 КБ, который Qt молча игнорирует), XMP (15 КБ у Эрмитажа) и ВЕСЬ каталог
# EXIF — диафрагму, выдержку, модель объектива. Qt не даёт из этого ничего.
#
# ПОЧЕМУ ВЕНДОРИНГ, А НЕ СИСТЕМНАЯ. Решение владельца по этапу: зависимости —
# компилятор C++, CMake и Qt, и ничего больше; программу переносить на Mac и
# Windows. Плата за это — обновления libtiff теперь наша забота, а история
# уязвимостей у неё длинная: следить за релизами придётся.
#
# CMake берём ЧУЖОЙ. Он порождает tif_config.h по проверкам платформы — какие
# заголовки есть, какие типы, какой порядок байт. Ради переносимости это ровно
# та работа, которую не стоит делать руками.
#
# Цена: дерево 2.3 МБ, 112 файлов.

set(TIFF_VENDORED_VERSION "4.7.0" PARENT_SCOPE)

macro(zametti_tiff_option name value)
    set(${name} ${value} CACHE BOOL "выставлено zametti" FORCE)
endmacro()

# Утилиты, тесты, примеры и установка — мимо.
zametti_tiff_option(tiff-tools      OFF)
zametti_tiff_option(tiff-tests      OFF)
zametti_tiff_option(tiff-contrib    OFF)
zametti_tiff_option(tiff-docs       OFF)
zametti_tiff_option(tiff-install    OFF)
zametti_tiff_option(tiff-deprecated OFF)

# Внешние кодеки: все выключены, кроме Deflate. LZW, PackBits, CCITT и «без
# сжатия» у libtiff внутренние, и в корпусе владельца ими сжаты 119 файлов из
# 120; сто двадцатый — Adobe Deflate, а им пишет Photoshop, так что это не
# редкость, а целый род.
zametti_tiff_option(zlib       ON)   # ради Deflate, через нашу 3rdparty/zlib
zametti_tiff_option(libdeflate OFF)
zametti_tiff_option(jpeg       OFF)  # JPEG внутри TIFF — отдельный разговор
zametti_tiff_option(old-jpeg   OFF)
zametti_tiff_option(jbig       OFF)
zametti_tiff_option(lzma       OFF)
zametti_tiff_option(zstd       OFF)
zametti_tiff_option(webp       OFF)
zametti_tiff_option(lerc       OFF)
zametti_tiff_option(cxx        OFF)  # обёртка C++ нам не нужна

# Статической, а не разделяемой: libtiff по умолчанию собирает .so, а нам
# нужен архив внутри программы — иначе при переносе на Mac и Windows придётся
# таскать библиотеку рядом, ради чего вендоринг и затевался. Обычной
# переменной, а не кэшем: она действует только на этот подкаталог и никого
# больше не задевает.
set(BUILD_SHARED_LIBS OFF)

add_subdirectory(upstream EXCLUDE_FROM_ALL)

# Одно имя наружу, как у остальных вендоренных.
add_library(libtiff INTERFACE)
target_link_libraries(libtiff INTERFACE tiff)

# Чужие заголовки — системные: иначе их предупреждения выписываются на наши
# файлы и при -Werror ломают сборку из-за чужого кода.
foreach(lib tiff)
    if(TARGET ${lib})
        get_target_property(dirs ${lib} INTERFACE_INCLUDE_DIRECTORIES)
        if(dirs)
            set_target_properties(${lib} PROPERTIES
                INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${dirs}")
        endif()
    endif()
endforeach()

# Чужой код чужими правилами: zametti-warnings сюда не цепляется.
