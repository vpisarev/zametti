# Просмотрщик. Единственная цель в проекте, которая знает про Qt.

find_package(Qt6 QUIET COMPONENTS Gui Widgets Svg)

if(NOT Qt6_FOUND)
    message(STATUS "Qt6 Widgets не найден — просмотрщик собран не будет "
                   "(ядру и его тестам Qt не нужен). Поставьте qt6-base-dev.")
    return()
endif()

# Svg отдельным пакетом, поэтому и проверка отдельная. Модуль, а не плагин
# imageformats: плагин лежит в необязательном qt6-svg-plugins, и на машине без
# него тулбар оказался бы пустым молча. Слинкованный модуль превращает
# зависимость в объявленную — забыть её при упаковке уже нельзя.
if(NOT TARGET Qt6::Svg)
    message(STATUS "Qt6 Svg не найден — просмотрщик собран не будет: без него "
                   "нечем рисовать иконки тулбара. Поставьте qt6-svg-dev.")
    return()
endif()

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

# Модель документа и её преобразования в обе стороны. Виджетов здесь нет
# намеренно: тесты этого слоя гоняются без дисплея и без QtWidgets, и проверяет
# это тест view-without-widgets.
add_library(zametti-view STATIC
    doc_model.h
    doc_model.cpp
    document_builder.h
    document_builder.cpp
    document_reader.h
    document_reader.cpp
    document_saver.h
    document_saver.cpp
    edit_history.h
    edit_history.cpp
    editor_ops.h
    editor_ops.cpp
    marker.h
    marker.cpp
    search.h
    search.cpp
    settings.h
    settings.cpp
    text_stats.h
    text_stats.cpp
)
# История правок живёт в хранилищной библиотеке, а зовёт её редактор — отсюда
# зависимость слоя модели на zametti-store-lib. Обратной нет и быть не должно:
# хранилище про виджеты не знает.
target_link_libraries(zametti-view PUBLIC zametti-core zametti-store-lib Qt6::Gui)
target_link_libraries(zametti-view PRIVATE zametti-warnings)
target_include_directories(zametti-view PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# Слой окна. Отдельной целью, а не прямо в исполняемом файле: тесту редактора
# нужны те же виджеты, а собирать их дважды незачем.
add_library(zametti-widgets STATIC
    find_bar.h
    find_bar.cpp
    history_panel.h
    history_panel.cpp
    note_tree.h
    note_tree.cpp
    note_list.h
    note_list.cpp
    search_results.h
    search_results.cpp
    store_search.h
    store_search.cpp
    note_view.h
    note_view.cpp
    editor_widget.h
    editor_widget.cpp
    image_insert.h
    image_insert.cpp
    icons.h
    icons.cpp
    image_facts.h
    image_facts.cpp
    image_importer.h
    image_importer.cpp
    about_window.h
    about_window.cpp
    status_bar.h
    status_bar.cpp
    toolbar.h
    toolbar.cpp
    resources.h
    resources.cpp
    ${CMAKE_SOURCE_DIR}/resources/resources.qrc
)
# Ресурсы (иконка, иконки тулбара, шрифты) собираются сюда, а не в исполняемый
# файл: тестам нужны те же шрифты и те же иконки, а набор, рисующий чужим
# шрифтом, проверял бы не то. Плата — Q_INIT_RESOURCE внутри resources.cpp:
# объектный файл ресурсов в статической библиотеке компоновщик вправе выбросить.
#
# Порог сжатия. Умолчание rcc (--threshold 70) требует, чтобы сжатое было меньше
# исходного на 70%, а ttf ужимается zstd на 48–63% — и восемь начертаний легли
# бы в бинарник как есть. Замер на нашем qrc: без ключа и с --no-compress
# одинаковые 1 723 331 байт, с порогом 30 — 852 558. Ниже тридцати ничего не
# меняется (порог 20 и 5 дают тот же байт), выше сорока начинает расти.
set_property(TARGET zametti-widgets APPEND PROPERTY AUTORCC_OPTIONS "--threshold;30")
# Свои читатели картинок (jxl, avif/heic) — статические плагины. Линкуются
# сюда, а не в исполняемый файл, потому что тестам редактора нужны те же
# форматы: набор, который не умеет прочитать jxl, проверял бы не то.
# build_facts.h порождает CMake рядом со сводкой настройки — см. корневой
# CMakeLists.txt. Каталог общий на все цели: тесту «О программе» нужны те же
# числа, что и окну.
target_include_directories(zametti-widgets PUBLIC ${CMAKE_BINARY_DIR}/generated)
target_link_libraries(zametti-widgets PUBLIC zametti-view zametti-imageio Qt6::Widgets Qt6::Svg)
target_link_libraries(zametti-widgets PRIVATE zametti-warnings)

add_executable(zametti
    main.cpp
)

target_link_libraries(zametti PRIVATE zametti-widgets zametti-store-lib zametti-warnings)

# Иконку в доке оболочка ищет не у окна, а по .desktop-файлу с именем, которое
# приложение сообщает через setDesktopFileName. Без установки этих двух файлов
# док показывает заглушку.
include(GNUInstallDirs)
install(TARGETS zametti RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES ${CMAKE_SOURCE_DIR}/packaging/zametti.desktop
        DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/applications)
install(FILES ${CMAKE_SOURCE_DIR}/resources/zametti.png
        DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/512x512/apps)
