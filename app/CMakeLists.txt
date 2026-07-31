# Просмотрщик. Единственная цель в проекте, которая знает про Qt.

find_package(Qt6 QUIET COMPONENTS Gui Widgets)

if(NOT Qt6_FOUND)
    message(STATUS "Qt6 Widgets не найден — просмотрщик собран не будет "
                   "(ядру и его тестам Qt не нужен). Поставьте qt6-base-dev.")
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
)
target_link_libraries(zametti-widgets PUBLIC zametti-view Qt6::Widgets)
target_link_libraries(zametti-widgets PRIVATE zametti-warnings)

add_executable(zametti
    main.cpp
    resources.qrc
)

target_link_libraries(zametti PRIVATE zametti-widgets zametti-store-lib zametti-warnings)

# Иконку в доке оболочка ищет не у окна, а по .desktop-файлу с именем, которое
# приложение сообщает через setDesktopFileName. Без установки этих двух файлов
# док показывает заглушку.
include(GNUInstallDirs)
install(TARGETS zametti RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES ${CMAKE_SOURCE_DIR}/packaging/zametti.desktop
        DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/applications)
install(FILES ${CMAKE_SOURCE_DIR}/art/zametti.png
        DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/512x512/apps)
