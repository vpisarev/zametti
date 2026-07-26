cmake_minimum_required(VERSION 3.20)
project(zametti LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 99)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "" FORCE)
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(ZAMETTI_WARNINGS_AS_ERRORS "Считать предупреждения ошибками" OFF)

add_library(zametti-warnings INTERFACE)
if(MSVC)
    target_compile_options(zametti-warnings INTERFACE /W4)
    if(ZAMETTI_WARNINGS_AS_ERRORS)
        target_compile_options(zametti-warnings INTERFACE /WX)
    endif()
else()
    target_compile_options(zametti-warnings INTERFACE -Wall -Wextra -Wpedantic)
    if(ZAMETTI_WARNINGS_AS_ERRORS)
        target_compile_options(zametti-warnings INTERFACE -Werror)
    endif()
endif()

enable_testing()

add_subdirectory(3rdparty/md4c)
add_subdirectory(zametti-core)
add_subdirectory(tests)
