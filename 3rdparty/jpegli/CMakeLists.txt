# jpegli, вендоринг подмножеством дерева. Взято из
# https://github.com/google/jpegli, коммит 031a0077 (1 июня 2026).
#
# РЕЛИЗОВ У ПРОЕКТА НЕТ ВОВСЕ — ни одного тега, поэтому закрепляем коммит и
# пишем его здесь. При обновлении переписать эту строку и прогнать замер
# точности заново.
#
# ЗАЧЕМ. jpegli — разжиматель JPEG, отдающий пиксели в шестнадцати битах, а не
# в восьми. JPEG у нас главный входной формат, и почти всякий раз картинку
# приходится уменьшать и пережимать; лишнее округление до байта по дороге
# теряется навсегда. Решение владельца — вести весь тракт импорта в 16 битах:
# декод, уменьшение, цветовые преобразования, а до двенадцати пусть ужимает уже
# сам JXL.
#
# ЧТО ЭТО ДАЁТ (замер, SSIMULACRA2 против лучшего доступного восстановления,
# путь: декод → уменьшение до 2.56 Мп → JXL q90 e5):
#
#   файл                    Qt/libjpeg  jpegli 8бит  jpegli 16бит
#   телефонный P3 (vivo)       88.60       89.67        89.89
#   работа художника, градиенты 84.51      84.71        85.11
#   iPhone P3, 38 Мп           88.47       88.44        88.80
#   зерно плёнки               87.54       87.73        87.81
#
# Вклад РАЗРЯДНОСТИ всегда положителен (+0.02…+0.40); вклад самого декодера
# местами уходит в минус, но на телефонном снимке даёт +1.07 — там цветность
# прорежена вчетверо (4:2:0), и её интерполяция у jpegli лучше. Вместе на
# телефонном P3 выходит +1.29 балла.
#
# Моя первая, синтетическая оценка была «около 0.1» и оказалась вчетверо
# занижена: она мерила только округление и не видела разницы декодеров.
#
# ПОЧЕМУ СВОЯ СБОРКА, А НЕ ЧУЖАЯ. У libjxl и jpegli общие предки, и их CMake
# завёл бы вторую цель skcms поверх той, что уже есть у libjxl, и вторую
# highway. Своих подмодулей мы не берём ни одного: highway общая
# (3rdparty/highway), skcms декодеру не нужна вовсе (проверено: в
# libjpegli-static.a ноль символов skcms), из libjpeg-turbo нужны три
# заголовка, и они лежат рядом.
#
# Цена: дерево 0.9 МБ, 96 файлов.

set(JPEGLI_VENDORED_COMMIT "031a0077" PARENT_SCOPE)

# Список исходников берём у самого jpegli: он порождается их скриптом, и
# переписывать его руками значило бы ломаться на каждом обновлении.
include(${CMAKE_CURRENT_SOURCE_DIR}/upstream/lib/jpegli_lists.cmake)

list(TRANSFORM JPEGLI_INTERNAL_JPEGLI_SOURCES PREPEND
     "${CMAKE_CURRENT_SOURCE_DIR}/upstream/lib/")
list(TRANSFORM JPEGLI_INTERNAL_BASE_SOURCES PREPEND
     "${CMAKE_CURRENT_SOURCE_DIR}/upstream/lib/")

# Заголовки libjpeg: jpegli реализует его API, и без них не соберётся ни он
# сам, ни тот, кто его зовёт. Ровно те же configure_file, что делает их
# собственный lib/jpegli.cmake, — вместе с подстановками, без которых в
# jconfig.h остаются пустые «#if ==» (наступал).
set(BITS_IN_JSAMPLE 8)
set(MEM_SRCDST_SUPPORTED 1)
set(JPEG_LIB_VERSION 62)          # их умолчание: SOVERSION "62"
set(LIBJPEG_TURBO_VERSION_NUMBER 2001090)
set(VERSION "2.1.90")
configure_file(upstream/third_party/libjpeg-turbo/jconfig.h.in include/jpegli/jconfig.h)
configure_file(upstream/third_party/libjpeg-turbo/jpeglib.h    include/jpegli/jpeglib.h COPYONLY)
configure_file(upstream/third_party/libjpeg-turbo/jmorecfg.h   include/jpegli/jmorecfg.h COPYONLY)
configure_file(upstream/third_party/libjpeg-turbo/jerror.h     include/jpegli/jerror.h COPYONLY)

add_library(jpegli STATIC
    ${JPEGLI_INTERNAL_JPEGLI_SOURCES}
    ${JPEGLI_INTERNAL_BASE_SOURCES}
)
set_target_properties(jpegli PROPERTIES POSITION_INDEPENDENT_CODE ON)

# Внутрь себя jpegli смотрит от корня своего дерева ("lib/jpegli/...") и в
# порождённые заголовки; наружу отдаёт только их.
target_include_directories(jpegli
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/upstream
    PUBLIC  ${CMAKE_CURRENT_BINARY_DIR}/include/jpegli
)
# Публичные заголовки jpegli лежат по путям вида "lib/jpegli/decode.h" — тем,
# кто его зовёт, нужен корень дерева. Системным, чтобы чужие предупреждения не
# выписывались на наши файлы.
target_include_directories(jpegli SYSTEM PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/upstream)

target_link_libraries(jpegli PUBLIC hwy)

# Чужой код чужими правилами: zametti-warnings (-Wall -Wextra -Wpedantic и, по
# ключу, -Werror) сюда НЕ цепляется. Тот же уговор, что с md4c, blake3, zstd,
# highway и libjxl.
