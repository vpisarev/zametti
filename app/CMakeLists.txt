# Просмотрщик. Единственная цель в проекте, которая знает про Qt.

find_package(Qt6 QUIET COMPONENTS Widgets)

if(NOT Qt6_FOUND)
    message(STATUS "Qt6 Widgets не найден — просмотрщик собран не будет "
                   "(ядру и его тестам Qt не нужен). Поставьте qt6-base-dev.")
    return()
endif()

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

add_executable(zametti
    main.cpp
    document_builder.h
    document_builder.cpp
    marker.h
    marker.cpp
    note_tree.h
    note_tree.cpp
    note_view.h
    note_view.cpp
    settings.h
    settings.cpp
    resources.qrc
)

target_link_libraries(zametti PRIVATE zametti-core Qt6::Widgets zametti-warnings)

# Иконку в доке оболочка ищет не у окна, а по .desktop-файлу с именем, которое
# приложение сообщает через setDesktopFileName. Без установки этих двух файлов
# док показывает заглушку.
include(GNUInstallDirs)
install(TARGETS zametti RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES ${CMAKE_SOURCE_DIR}/packaging/zametti.desktop
        DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/applications)
install(FILES ${CMAKE_SOURCE_DIR}/art/zametti.png
        DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/512x512/apps)
