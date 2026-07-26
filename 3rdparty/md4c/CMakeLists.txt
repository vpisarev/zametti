# md4c 0.5.3, вендоринг. Взято из
# https://github.com/mity/md4c/archive/refs/tags/release-0.5.3.tar.gz — только
# src/md4c.c и src/md4c.h: ни html-рендерер, ни таблица сущностей ядру не нужны.
# Лицензия — MIT, см. LICENSE.md рядом.

add_library(md4c STATIC md4c.c)
target_include_directories(md4c PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(md4c PROPERTIES POSITION_INDEPENDENT_CODE ON)
