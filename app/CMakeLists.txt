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
    checkbox_object.h
    checkbox_object.cpp
    settings.h
    settings.cpp
    resources.qrc
)

target_link_libraries(zametti PRIVATE zametti-core Qt6::Widgets zametti-warnings)
