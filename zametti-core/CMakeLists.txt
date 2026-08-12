# Ядро формата: markdown → IR → markdown.
#
# Жёсткое требование: ни одного include из QtWidgets. Зависимость от Qt здесь
# отсутствует целиком — проверяется тем, что цель не линкуется ни с чем, кроме
# md4c и blake3 (обе на C), и тестовый бинарник собирается без Qt в принципе.

add_library(zametti-core STATIC
    ir.h
    ir.cpp
    meta.cpp
    parser.h
    parser.cpp
    serializer.h
    serializer.cpp
    table.h
    table.cpp
    json_dump.h
    json_dump.cpp
    note_id.h
    note_id.cpp
    hash.h
    hash.cpp
)

target_include_directories(zametti-core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(zametti-core PRIVATE md4c blake3 zametti-warnings)
set_target_properties(zametti-core PROPERTIES POSITION_INDEPENDENT_CODE ON)
