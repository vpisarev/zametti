# Свои читатели картинок: JPEG XL поверх вендоренной libjxl и AVIF/HEIC
# поверх системной libheif.
#
# Зачем свои. JXL — основной формат хранения zametti, и его декод обязан быть
# одинаковым везде; дистрибутивный плагин где-то есть, где-то нет. Для
# AVIF/HEIC плагина под Qt6 в дистрибутиве нет ВОВСЕ (qt6-image-formats-plugins
# даёт tiff/webp/tga/mng/icns/wbmp, kimageformats под Qt6 в apt отсутствует), а
# шесть файлов матрицы перекодирования — как раз avif и heic.
#
# Плагины СТАТИЧЕСКИЕ. Динамический пришлось бы куда-то устанавливать и
# надеяться, что Qt его найдёт, — ровно та зависимость от окружения, от которой
# мы и уходим. Статический линкуется в программу: слинковали — значит работает.
#
# Цель отдельная, не внутри app/, потому что читатели нужны троим: просмотрщику,
# утилите хранилища (recompress разжимает вложения) и наборам тестов.

# --- метаданные картинок ---------------------------------------------------
#
# ОТДЕЛЬНОЙ целью и БЕЗ Qt, хотя всё остальное в этом каталоге на нём. Причина
# в потребителях: метаданные нужны и утилите хранилища (recompress), а она
# живёт на Qt6::Core и тащить ради этого весь Qt6::Gui незачем.
#
# Здесь обход контейнеров — вынуть блоб оттуда, где он лежит: маркеры JPEG,
# чанки RIFF у WebP, чанки PNG. У TIFF, HEIF и JXL метаданные забирают их
# собственные читатели, каждый своим способом.
#
# Сперва этот файл лежал в zametti-core — и это было неправильно: ядро о себе
# говорит «markdown → IR → markdown», а EXIF там ни при чём. Рассуждение было
# про зависимости («чистые байты, значит можно в ядро»), а надо было про
# принадлежность.
add_library(zametti-imagemeta STATIC exif.cpp exif.h import_limits.cpp import_limits.h)
target_include_directories(zametti-imagemeta PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(zametti-imagemeta PRIVATE zametti-warnings)
set_target_properties(zametti-imagemeta PROPERTIES POSITION_INDEPENDENT_CODE ON)

find_package(Qt6 QUIET COMPONENTS Gui)
if(NOT Qt6_FOUND)
    message(STATUS "Qt6 Gui не найден — свои читатели картинок собраны не будут")
    return()
endif()

set(CMAKE_AUTOMOC ON)

# --- цвет ------------------------------------------------------------------
#
# Отдельной целью, потому что нужен ОБОИМ концам: записи — чтобы привести
# картинку к пространству, которое переживёт круг, и чтению — чтобы вернуть
# пиксели чужого файла из линейного пространства libjxl в его исходное.
#
# Движком служит CMS самой libjxl (skcms). Не Qt: на табличных профилях вроде
# «Apple Wide Color Sharing Profile» Qt строит преобразование по угаданным
# первичным и ошибается втрое (замер: 53/50/42 он переводит в 37/35/28, тогда
# как littleCMS — в 47/44/36).
add_library(zametti-color STATIC color.cpp color.h)
target_link_libraries(zametti-color PUBLIC Qt6::Gui libjxl PRIVATE zametti-warnings)
target_include_directories(zametti-color PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

qt_add_plugin(zametti-jxl STATIC
    CLASS_NAME JxlPlugin
    PLUGIN_TYPE imageformats
)
target_sources(zametti-jxl PRIVATE jxl_plugin.cpp jxl_handler.cpp)
target_link_libraries(zametti-jxl PRIVATE Qt6::Gui libjxl zametti-color zametti-warnings)
target_include_directories(zametti-jxl PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# --- AVIF и HEIC -----------------------------------------------------------
#
# libheif системная, не вендоренная: она тянет за собой кодеки видео (aom или
# de265/x265), и вендоринг этого хозяйства обошёлся бы дороже самой libjxl, а
# AVIF для нас формат ВХОДА, не хранения. Нет libheif — нет плагина, и вставка
# avif/heic честно откажет; тихого пропуска не будет.

find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(LIBHEIF IMPORTED_TARGET libheif)
endif()

if(LIBHEIF_FOUND)
    qt_add_plugin(zametti-heif STATIC
        CLASS_NAME HeifPlugin
        PLUGIN_TYPE imageformats
    )
    target_sources(zametti-heif PRIVATE heif_plugin.cpp heif_handler.cpp)
    target_link_libraries(zametti-heif PRIVATE Qt6::Gui PkgConfig::LIBHEIF zametti-warnings)
    target_include_directories(zametti-heif PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    set(ZAMETTI_HAVE_HEIF ON PARENT_SCOPE)
    set(ZAMETTI_HEIF_VERSION "${LIBHEIF_VERSION}" PARENT_SCOPE)
else()
    message(STATUS "libheif не найдена — avif и heic читаться не будут. "
                   "Поставьте libheif-dev.")
    set(ZAMETTI_HAVE_HEIF OFF PARENT_SCOPE)
endif()

# --- TIFF ------------------------------------------------------------------
#
# НЕ плагин, а обычный читатель. Плагином он быть не может и не должен: нам
# нужны не только пиксели, но и сырые отсчёты, цветовое пространство, ICC, XMP
# и EXIF, а интерфейс QImageIOHandler ничего этого не передаёт. Плагин Qt для
# TIFF остаётся на месте и обслуживает показ; импорт идёт мимо него, потому что
# он тихо перевирает CIELab.
# Запись JXL: единственное место, где картинка превращается в то, что ляжет в
# хранилище. Отдельной целью от плагина чтения — читают у нас все, а пишет
# только импорт.
add_library(zametti-jxl-encode STATIC jxl_encoder.cpp jxl_encoder.h
                                     resample.cpp resample.h)
target_link_libraries(zametti-jxl-encode PUBLIC Qt6::Gui libjxl zametti-color
                      PRIVATE zametti-warnings)

# Ресемплер раскладывается по строкам без связей между ними — параллелится
# даром. OpenMP берём, ТОЛЬКО если компилятор его даёт: на macOS с AppleClang
# он требует отдельной libomp, а зависеть от неё нельзя (правило владельца —
# компилятор, CMake и Qt, и ничего больше). Нет OpenMP — всё работает так же,
# только в один поток.
find_package(OpenMP QUIET COMPONENTS CXX)
if(OpenMP_CXX_FOUND)
    target_link_libraries(zametti-jxl-encode PRIVATE OpenMP::OpenMP_CXX)
    target_link_libraries(zametti-color PRIVATE OpenMP::OpenMP_CXX)
    set(ZAMETTI_HAVE_OPENMP ON PARENT_SCOPE)
else()
    set(ZAMETTI_HAVE_OPENMP OFF PARENT_SCOPE)
endif()
target_include_directories(zametti-jxl-encode PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

add_library(zametti-tiff STATIC tiff_reader.cpp tiff_reader.h)
# zametti-color — не украшение: CMYK переводится по вложенному профилю тем же
# движком, что и всё остальное (см. tiff_reader.cpp).
target_link_libraries(zametti-tiff PUBLIC Qt6::Gui libtiff zametti-color
                      PRIVATE zametti-warnings)
target_include_directories(zametti-tiff PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# --- конвейер вставки ------------------------------------------------------
#
# Таблица решений и всё, что вокруг неё. Собирает вместе уже проверенные по
# отдельности части: границы, чтение TIFF, метаданные, запись JXL, ресемплер.
add_library(zametti-import STATIC import.cpp import.h ladder.cpp ladder.h
                                  recompress.cpp recompress.h)
# libjxl нужна напрямую: арбитр SSIMULACRA2 живёт в её внутренностях.
target_link_libraries(zametti-import
    PUBLIC Qt6::Gui zametti-jxl-encode zametti-tiff zametti-imagemeta libjxl
    PRIVATE zametti-warnings)
target_include_directories(zametti-import PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# Одна цель на всё: потребителю незачем знать, из скольких плагинов состоят
# наши читатели и какие из них собрались.
add_library(zametti-imageio INTERFACE)
target_link_libraries(zametti-imageio INTERFACE zametti-jxl zametti-jxl-encode
                      zametti-color zametti-tiff zametti-imagemeta zametti-import)
if(LIBHEIF_FOUND)
    target_link_libraries(zametti-imageio INTERFACE zametti-heif)
endif()
