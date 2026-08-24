# Поиск системного libsodium (решение владельца: НЕ вендорим — библиотека
# повсеместна в Ubuntu и brew, а Android-порт вернётся к вопросу, когда
# наступит). Даёт цель Sodium::Sodium и переменные Sodium_FOUND,
# Sodium_VERSION.
#
# Сначала pkg-config — он знает и версию, и нестандартные префиксы; там, где
# его нет (мак без pkg-config), — find_path/find_library с разбором версии из
# sodium/version.h. Требовать pkg-config было бы лишней зависимостью ради
# одной строки.

find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(PC_SODIUM QUIET libsodium)
endif()

find_path(Sodium_INCLUDE_DIR sodium.h HINTS ${PC_SODIUM_INCLUDE_DIRS})
find_library(Sodium_LIBRARY NAMES sodium libsodium HINTS ${PC_SODIUM_LIBRARY_DIRS})

if(PC_SODIUM_VERSION)
    set(Sodium_VERSION "${PC_SODIUM_VERSION}")
elseif(Sodium_INCLUDE_DIR AND EXISTS "${Sodium_INCLUDE_DIR}/sodium/version.h")
    file(STRINGS "${Sodium_INCLUDE_DIR}/sodium/version.h" sodium_version_line
         REGEX "#define[ \t]+SODIUM_VERSION_STRING[ \t]+\"")
    if(sodium_version_line MATCHES "\"([0-9.]+)\"")
        set(Sodium_VERSION "${CMAKE_MATCH_1}")
    endif()
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Sodium
    REQUIRED_VARS Sodium_LIBRARY Sodium_INCLUDE_DIR
    VERSION_VAR Sodium_VERSION)

if(Sodium_FOUND AND NOT TARGET Sodium::Sodium)
    add_library(Sodium::Sodium UNKNOWN IMPORTED)
    set_target_properties(Sodium::Sodium PROPERTIES
        IMPORTED_LOCATION "${Sodium_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${Sodium_INCLUDE_DIR}")
endif()

mark_as_advanced(Sodium_INCLUDE_DIR Sodium_LIBRARY)
