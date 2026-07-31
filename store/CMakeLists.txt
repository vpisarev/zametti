# Утилита плоского хранилища. Библиотека отдельно от main: тесты зовут функции
# напрямую. Ядро остаётся без Qt; здесь Qt6::Core — JSON манифеста и sha256
# вложений.
find_package(Qt6 REQUIRED COMPONENTS Core)

add_library(zametti-store-lib STATIC
    store.h
    store.cpp
    journal.h
    journal.cpp
)
target_include_directories(zametti-store-lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(zametti-store-lib PUBLIC zametti-core Qt6::Core
                      PRIVATE zstd zametti-warnings)

add_executable(zametti-store main.cpp)
target_link_libraries(zametti-store PRIVATE zametti-store-lib zametti-warnings)
