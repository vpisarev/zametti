# BLAKE3 1.5.4, вендоринг. Взято из
# https://github.com/BLAKE3-team/BLAKE3/archive/refs/tags/1.5.4.tar.gz — только
# портативная часть эталонной реализации на C: пять файлов из c/.
# Лицензия — CC0 (двойная, вторая Apache-2.0), см. LICENSE рядом.
#
# Зачем именно BLAKE3, а не что попроще: хеш нужен один на весь проект — им
# сверяется файл при возврате в закэшированную заметку, им же решается «не
# писать, если не изменилось», и позже им же будут проверяться журнал и
# синхронизация. Он криптостойкий, поэтому границы применимости помнить не
# надо. Тот же алгоритм вендорит ccache.
#
# Собираем БЕЗ SIMD. Ускорители тянут за собой пять дополнительных файлов,
# разбор возможностей процессора во время работы и сборочные условия под каждую
# архитектуру — за наши объёмы это не окупается. Замер портативной версии
# (RelWithDebInfo, taskset -c 0, медиана из 30):
#
#   1336 байт (медианная заметка)      2.6 мкс    520 МБ/с
#   60 КБ                            111 мкс      544 МБ/с
#   239 КБ (самая большая заметка)   565 мкс      423 МБ/с
#
# В отладочной сборке она же идёт 37 МБ/с — там хеш большой заметки стоит
# 6.5 мс. Это цена -O0, а не алгоритма; удивляться в профиле нечему.
set(BLAKE3_VENDORED_VERSION "1.5.4" PARENT_SCOPE)

add_library(blake3 STATIC blake3.c blake3_dispatch.c blake3_portable.c)
target_include_directories(blake3 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(blake3 PRIVATE
    BLAKE3_NO_SSE2 BLAKE3_NO_SSE41 BLAKE3_NO_AVX2 BLAKE3_NO_AVX512 BLAKE3_NO_NEON)
set_target_properties(blake3 PROPERTIES POSITION_INDEPENDENT_CODE ON)
