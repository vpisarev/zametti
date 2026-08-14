# Свои читатели картинок: jxl (libjxl), jpeg (jpegli), tiff (libtiff),
# webp (libwebp), heif/avif (системная libheif, по ключу WITH_HEIF).
#
# ЗАЧЕМ СВОИ И ПОЧЕМУ БЕЗ ПЛАГИНОВ. Формат хранения заметок не имеет права
# зависеть от того, как собрали чужую библиотеку. Замер: если у Qt отнять
# каталог плагинов, он читает только bmp, png, p*m и x*m — jpeg, tiff и webp
# у него тоже плагины, а плагина AVIF/HEIC под Qt6 в дистрибутиве нет вовсе.
#
# Поэтому реестра Qt в наших путях больше нет ни одного: развилка по подписи
# и прямой вызов (см. image_read.h). Чужие форматы, которых мы не знаем,
# по-прежнему падают в QImage::fromData — там Qt волен звать свои плагины,
# если они есть. Мы на них не опираемся, но и не отказываемся назло: чужой
# файл человек вправе принести любой.
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
target_link_libraries(zametti-imagemeta PRIVATE zametti-code-rules)
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
target_link_libraries(zametti-color PUBLIC Qt6::Gui libjxl PRIVATE zametti-code-rules)
target_include_directories(zametti-color PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# Плагинов у нас БОЛЬШЕ НЕТ. Чтение JXL переехало в jxl_read (см. image_read.h):
# оно и раньше было нашим кодом, а QImageIOHandler вокруг служил только реестру
# Qt. Реестр же оказался ловушкой: замер показал, что БЕЗ плагинов Qt читает
# лишь bmp, png, p*m и x*m, то есть формат хранения заметок зависел бы от того,
# как собрали чужую библиотеку.

# --- AVIF и HEIC -----------------------------------------------------------
#
# libheif системная, не вендоренная: она тянет за собой кодеки видео (aom или
# de265/x265), и вендоринг этого хозяйства обошёлся бы дороже самой libjxl, а
# AVIF для нас формат ВХОДА, не хранения. Нет плагина — вставка avif/heic
# честно откажет; тихого пропуска не будет.
#
# Берётся ТОЛЬКО по ключу WITH_HEIF (см. обоснование в корневом CMakeLists) и
# по умолчанию выключена. Раньше она бралась молча, если нашлась, — и сборка
# получалась разной на разных машинах, не сказав об этом ни слова.
if(WITH_HEIF)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(LIBHEIF IMPORTED_TARGET libheif)
    endif()

    # Просили явно — значит молчать нельзя. Прежний тихий пропуск оставлял
    # человека с программой, которая не читает то, что он для неё включал.
    if(NOT LIBHEIF_FOUND)
        if(APPLE)
            set(hint "brew install libheif")
        elseif(WIN32)
            set(hint "vcpkg install libheif (и путь через CMAKE_PREFIX_PATH)")
        else()
            set(hint "sudo apt install libheif-dev")
        endif()
        message(FATAL_ERROR
            "WITH_HEIF=ON, а libheif не найдена (pkg-config её не видит).\n"
            "  Поставьте: ${hint}\n"
            "  Либо соберите без avif/heic: -DWITH_HEIF=OFF")
    endif()

    # Обычная библиотека, не плагин: обработчик HEIF зовётся напрямую из
    # image_read, минуя реестр Qt. Разбирать его на probe/decode, как JXL, не
    # стали — он написан, работает и трогать его ради единообразия незачем.
    add_library(zametti-heif STATIC heif_handler.cpp heif_handler.h)
    target_link_libraries(zametti-heif PRIVATE Qt6::Gui PkgConfig::LIBHEIF zametti-code-rules)
    target_include_directories(zametti-heif PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    set(ZAMETTI_HAVE_HEIF ON PARENT_SCOPE)
    set(ZAMETTI_HEIF_VERSION "${LIBHEIF_VERSION}" PARENT_SCOPE)
else()
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
                      PRIVATE zametti-code-rules)

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
                      PRIVATE zametti-code-rules)
target_include_directories(zametti-tiff PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# --- конвейер вставки ------------------------------------------------------
#
# Таблица решений и всё, что вокруг неё. Собирает вместе уже проверенные по
# отдельности части: границы, чтение TIFF, метаданные, запись JXL, ресемплер.
add_library(zametti-import STATIC import.cpp import.h ladder.cpp ladder.h
                                  recompress.cpp recompress.h
                                  image_read.cpp image_read.h
                                  jpeg_read.cpp jpeg_read.h
                                  jxl_read.cpp jxl_read.h
                                  webp_read.cpp webp_read.h)
# libjxl нужна напрямую: арбитр SSIMULACRA2 живёт в её внутренностях.
# jpegli — свой разжиматель JPEG (шестнадцать бит, см. jpeg_read.h),
# libwebp — свой webp: оба вместо чужих плагинов Qt.
target_link_libraries(zametti-import
    PUBLIC Qt6::Gui zametti-jxl-encode zametti-tiff zametti-imagemeta libjxl
    PRIVATE zametti-code-rules jpegli libwebp)
if(TARGET zametti-heif)
    target_link_libraries(zametti-import PRIVATE zametti-heif)
    target_compile_definitions(zametti-import PRIVATE ZAMETTI_HAVE_HEIF)
endif()
target_include_directories(zametti-import PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# Одна цель на всё: потребителю незачем знать, из скольких библиотек состоят
# наши читатели и какие из них собрались.
add_library(zametti-imageio INTERFACE)
target_link_libraries(zametti-imageio INTERFACE zametti-jxl-encode
                      zametti-color zametti-tiff zametti-imagemeta zametti-import)
# Спрашиваем ЦЕЛЬ, а не LIBHEIF_FOUND: та переменная приходит из кэша
# pkg-config и переживает выключение ключа. С ней сборка после
# `-DWITH_HEIF=OFF` в готовом каталоге падала на `cannot find -lzametti-heif`:
# цели уже нет, а строка компоновки её всё ещё звала.
if(TARGET zametti-heif)
    target_link_libraries(zametti-imageio INTERFACE zametti-heif)
endif()
