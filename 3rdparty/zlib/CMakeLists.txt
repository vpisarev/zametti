# zlib 1.3.1, вендоринг. Взято из https://github.com/madler/zlib, тег v1.3.1 —
# только исходники верхнего уровня, без примеров, тестов и обвязки для сборки.
#
# ЗАЧЕМ. Один-единственный потребитель — libtiff, и только ради одного кодека:
# Adobe Deflate. Без zlib libtiff умеет LZW, PackBits и «без сжатия», а Deflate
# не умеет вовсе.
#
# Казалось бы, мелочь: в 120 файлах TIFF у владельца Deflate ровно один. Но это
# не редкость, а целый род — «ZIP» в диалоге Photoshop пишет именно им, и
# наткнуться на такой файл проще простого. Правило владельца: картинки должны
# кушаться всякие, особенно когда это недорого.
#
# ПОЧЕМУ ВЕНДОРИНГ. Решение владельца по всему этапу: список зависимостей —
# компилятор C++, CMake и Qt, и ничего больше. Программу переносить на Mac и
# Windows, а системная zlib там либо своя, либо никакая.
#
# Цена: 1.1 МБ, 26 файлов, ни одной собственной зависимости.
#
# Своя сборка, а не CMake самой zlib: та заводит цели установки, примеры и
# тесты, переименовывает библиотеку под платформу и умеет удивлять. Нам нужен
# один статический архив из пятнадцати файлов.

set(ZLIB_VENDORED_VERSION "1.3.1" PARENT_SCOPE)

# Файлы gz*.c намеренно НЕ берём (заголовок gzguts.h остаётся: его включает
# zutil.c). Они дают работу с .gz-файлами через POSIX-овые read/write/lseek,
# требуют признака unistd.h и на каждой платформе ведут себя по-своему. Нам
# zlib нужна ровно для одного — потокового Deflate внутри TIFF, а туда
# gzip-обёртка не входит вовсе.
add_library(zlib STATIC
    adler32.c compress.c crc32.c deflate.c
    infback.c inffast.c inflate.c inftrees.c
    trees.c uncompr.c zutil.c
)
target_include_directories(zlib SYSTEM PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(zlib PROPERTIES POSITION_INDEPENDENT_CODE ON)

# libtiff ищет zlib через find_package(ZLIB) и пользуется целью ZLIB::ZLIB —
# отдаём ей псевдоним, чтобы её CMake остался нетронутым.
if(NOT TARGET ZLIB::ZLIB)
    add_library(ZLIB::ZLIB ALIAS zlib)
endif()

# И заполняем кэш заранее, иначе find_package(ZLIB) нашёл бы СИСТЕМНУЮ, а весь
# смысл вендоринга — не зависеть от того, что стоит на машине. Тот же приём,
# что с highway; чужой CMake при этом не тронут.
set(ZLIB_FOUND TRUE CACHE BOOL "выставлено zametti" FORCE)
set(ZLIB_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}"
    CACHE PATH "выставлено zametti: zlib живёт в дереве" FORCE)
set(ZLIB_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}" CACHE PATH "выставлено zametti" FORCE)
set(ZLIB_LIBRARY "$<TARGET_FILE:zlib>" CACHE FILEPATH "выставлено zametti" FORCE)
set(ZLIB_LIBRARIES zlib CACHE STRING "выставлено zametti" FORCE)
set(ZLIB_VERSION_STRING "1.3.1" CACHE STRING "выставлено zametti" FORCE)

# Чужой код чужими правилами: zametti-code-rules сюда не цепляется. zlib старая,
# и на нынешних компиляторах предупреждений у неё хватает.
