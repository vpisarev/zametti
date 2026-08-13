# MicroTeX — движок вёрстки формул, ветка openmath.
#
# Откуда, какой коммит и что выброшено — см. update.sh рядом. Разведка,
# по итогам которой движок выбран: docs/zametti-brief-m16.md и отчёт
# `zametti-microtex-final-report.md` (98 формул корпуса, 0 падений, медиана
# рендера 0.017 мс).
#
# Цена: 1,1 МБ на диске, 173 файла, 21 202 строки. Взято только ЯДРО И
# QT-РИСОВАЛКА (решение владельца): ни бэкендов cairo/gtk/skia/wasm, ни
# примеров, ни бандла шрифтов XITS — свои шрифты лежат в resources/fonts/math.
# Сверх Qt6::Gui не требует ничего: ни tinyxml2, ни freetype, ни fontconfig —
# разбор OpenType и .clm у него свой.
#
# Чужой CMake не правим (правило §4 CLAUDE.md): всё, что нужно, продавливаем
# в кэш ДО add_subdirectory. Ниже — почему каждая из опций именно такая.

set(MICROTEX_VENDORED_VERSION "1.0.0 (openmath @086f4eb, 2024-08-05)" PARENT_SCOPE)

macro(zmath_microtex_option name value)
    set(${name} ${value} CACHE BOOL "выставлено zametti" FORCE)
endmacro()

# Единственный нужный бэкенд. Остальные каталоги platform/ вообще не завезены.
zmath_microtex_option(QT ON)
zmath_microtex_option(CAIRO OFF)
zmath_microtex_option(GTK OFF)
zmath_microtex_option(GDI OFF)
zmath_microtex_option(SKIA OFF)
zmath_microtex_option(WASM OFF)

# Их примеры нам не нужны, свой стенд в src/.
zmath_microtex_option(BUILD_EXAMPLE_QT OFF)
zmath_microtex_option(BUILD_EXAMPLE_QTPNG OFF)
zmath_microtex_option(BUILD_EXAMPLE_GTKMM OFF)
zmath_microtex_option(BUILD_EXAMPLE_GTK OFF)
zmath_microtex_option(BUILD_EXAMPLE_WIN32 OFF)
zmath_microtex_option(BUILD_EXAMPLE_MEMCHECK OFF)

# ВАЖНО. У них GRAPHICS_DEBUG по умолчанию ON — это рисует отладочные рамки
# вокруг каждого бокса поверх формулы. С ним ни о каком «красиво» речи нет.
zmath_microtex_option(GRAPHICS_DEBUG OFF)

# ВАЖНО. HAVE_AUTO_FONT_FIND тянет try_compile на файле
# ${CMAKE_SOURCE_DIR}/test/has_filesystem.cpp. CMAKE_SOURCE_DIR — это КОРЕНЬ
# ВСЕЙ сборки, то есть наш, а не их; файла там нет, проверка молча
# проваливается, и код уходит в find_package(Boost REQUIRED COMPONENTS
# filesystem) — жёсткая ошибка на ровном месте. Авто-поиск шрифтов нам не
# нужен: пути к .clm и .otf стенд задаёт явно.
zmath_microtex_option(HAVE_AUTO_FONT_FIND OFF)

# Лог движка пишется в stderr на каждый глиф; на прогоне корпуса это шум.
zmath_microtex_option(HAVE_LOG OFF)

# Статикой, как и всё вендоренное в zametti. У них своя переменная вместо
# BUILD_SHARED_LIBS: option(BUILD_STATIC) -> _BUILD_STATIC.
zmath_microtex_option(BUILD_STATIC ON)

# Тип отрисовки глифов: 0 — и контурами, и шрифтом (переключается в рантайме
# через MicroTeX::setRenderGlyphUsePath), 1 — только контурами, 2 — только
# шрифтом. Оставляем 0 именно ради возможности сравнить оба режима на корпусе.
set(GLYPH_RENDER_TYPE 0 CACHE STRING "выставлено zametti" FORCE)

add_subdirectory(upstream)

# Одно имя наружу, как у прочего вендоренного. microtex-qt линкует microtex
# приватно, поэтому в фасаде нужны оба.
add_library(microtex-all INTERFACE)
target_link_libraries(microtex-all INTERFACE microtex microtex-qt)

# Чужие заголовки — системные: иначе их предупреждения выписываются на наши
# файлы и при -Werror ломают сборку из-за чужого кода.
foreach(lib microtex microtex-qt)
    get_target_property(dirs ${lib} INTERFACE_INCLUDE_DIRECTORIES)
    if(dirs)
        set_target_properties(${lib} PROPERTIES
            INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${dirs}")
    endif()
endforeach()

# Шрифты движку НЕ ВОЗИМ: свои (Euler + Latin Modern) лежат в
# resources/fonts/math и уезжают в qrc. Qt-бэкенд умеет читать из ресурсов сам
# — если файла нет на диске, он приписывает к имени ":/" (graphic_qt.cpp,
# Font_qt::loadFont), а .clm2 мы отдаём байтами через FontSrcData.
