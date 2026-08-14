# От каталога осталась одна забота — системная libheif, и то по ключу.
#
# Всё остальное переехало в zametti-core/image: свои читатели картинок больше
# не отдельный слой, а часть ядра. Отдельной целью они были ради утилиты
# хранилища, которой хватало Qt6::Core; теперь ядро всё равно зависит от
# QtGui, и делить нечего.
#
# Здесь же оставлена и причина, по которой мы вообще ушли от плагинов: замер
# показал, что без каталога плагинов Qt читает лишь bmp, png, p*m и x*m — jpeg,
# tiff и webp у него тоже плагины, а плагина AVIF/HEIC под Qt6 в дистрибутиве
# нет вовсе. Формат хранения заметок не имеет права зависеть от того, как
# собрали чужую библиотеку.

# AVIF и HEIC. ВЫКЛЮЧЕНО по умолчанию: libheif тянет за собой видеокодеки
# (aom либо de265/x265), а с ними — их зависимости. AVIF/HEIC у нас формат
# ВХОДА, не хранения: без них программа теряет ввоз двух форматов и ничего
# больше. Нет библиотеки — вставка avif/heic честно откажет, тихого пропуска
# не будет.
if(NOT WITH_HEIF)
    set(ZAMETTI_HAVE_HEIF OFF PARENT_SCOPE)
    return()
endif()

find_package(Qt6 QUIET COMPONENTS Gui)
if(NOT Qt6_FOUND)
    set(ZAMETTI_HAVE_HEIF OFF PARENT_SCOPE)
    return()
endif()

find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(LIBHEIF IMPORTED_TARGET libheif)
endif()

# Просили явно — значит молчать нельзя. Прежний тихий пропуск оставлял человека
# с программой, которая не читает то, что он для неё включал.
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

set(CMAKE_AUTOMOC ON)

# Обычная библиотека, не плагин: обработчик зовётся напрямую из image_read,
# минуя реестр Qt. Разбирать его на probe/decode, как JXL, не стали — он
# написан, работает, и трогать его ради единообразия незачем.
add_library(zametti-heif STATIC heif_handler.cpp heif_handler.h)
target_link_libraries(zametti-heif PRIVATE Qt6::Gui PkgConfig::LIBHEIF zametti-code-rules)
target_include_directories(zametti-heif PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
set(ZAMETTI_HAVE_HEIF ON PARENT_SCOPE)
set(ZAMETTI_HEIF_VERSION "${LIBHEIF_VERSION}" PARENT_SCOPE)
