# A mechanical check of the owner's decision (07.09.2026): THE PROGRAM NEVER
# THINS THE JOURNALS ON ITS OWN. The thinning pass that used to run at start
# read every journal three times, contended with the editor for the journal
# lock, and dropped records without voiding them — under sync they came back.
# The only door is `zametti store thin`, run by a person.
#
# The guard: `thinAllJournals(` may be called only from the CLI and defined
# only in the store; the window (main.cpp) and everything else must not know
# the name. Suites are free to call it.

if(NOT SOURCE_DIRS)
    message(FATAL_ERROR "the guard was not given SOURCE_DIRS")
endif()

set(OFFENDERS "")
set(CHECKED 0)

foreach(dir ${SOURCE_DIRS})
    if(NOT IS_DIRECTORY "${dir}")
        continue()
    endif()
    file(GLOB_RECURSE SOURCES "${dir}/*.h" "${dir}/*.cpp")
    foreach(src ${SOURCES})
        get_filename_component(name "${src}" NAME)
        # The store defines it, the CLI is the door.
        if(name STREQUAL "zstorage.h" OR name STREQUAL "zstorage.cpp" OR name STREQUAL "store_cli.cpp")
            continue()
        endif()
        math(EXPR CHECKED "${CHECKED} + 1")
        file(STRINGS "${src}" LINES ENCODING UTF-8)
        set(n 0)
        foreach(line IN LISTS LINES)
            math(EXPR n "${n} + 1")
            if(line MATCHES "thinAllJournals[ ]*\\(" OR line MATCHES "thinJournal[ ]*\\(")
                list(APPEND OFFENDERS "${src}:${n}: ${line}")
            endif()
        endforeach()
    endforeach()
endforeach()

if(OFFENDERS)
    message(STATUS "journal thinning is called outside the CLI:")
    foreach(o ${OFFENDERS})
        message(STATUS "  ${o}")
    endforeach()
    message(FATAL_ERROR "the program never thins journals on its own; the only door is `zametti store thin` (the owner, 07.09.2026)")
endif()
message(STATUS "journal thinning: checked ${CHECKED} files, only the CLI calls it")
