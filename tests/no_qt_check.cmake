# Механическая проверка требования из брифа: в ядре не должно быть ни одного
# include из Qt. Тестовый бинарник и так собирается без Qt, но это проверяет
# намерение, а не только текущее состояние сборки.

file(GLOB_RECURSE ZAMETTI_CORE_SOURCES "${CORE_DIR}/*.h" "${CORE_DIR}/*.cpp")
set(OFFENDERS "")
foreach(src ${ZAMETTI_CORE_SOURCES})
    file(STRINGS "${src}" HITS REGEX "^[ \t]*#[ \t]*include[ \t]*[<\"]Q")
    if(HITS)
        list(APPEND OFFENDERS "${src}: ${HITS}")
    endif()
endforeach()

if(OFFENDERS)
    message(FATAL_ERROR "в ядре найдены include из Qt:\n${OFFENDERS}")
endif()
message(STATUS "ядро свободно от Qt: проверено файлов — ${ZAMETTI_CORE_SOURCES}")
