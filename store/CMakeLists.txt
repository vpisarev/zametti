# Утилита плоского хранилища. Библиотека отдельно от main: тесты зовут функции
# напрямую. Ядро остаётся без Qt; здесь Qt6::Core — JSON манифеста и sha256
# вложений.
find_package(Qt6 REQUIRED COMPONENTS Core)

add_library(zametti-store-lib STATIC
    store.h
    store.cpp
    journal.h
    journal.cpp
    history_rules.h
    history_rules.cpp
    times.h
    times.cpp
    archive.h
    archive.cpp
    lost_found.h
    lost_found.cpp
)
target_include_directories(zametti-store-lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(zametti-store-lib PUBLIC zametti-core Qt6::Core
                      PRIVATE zstd zametti-code-rules)

# Пережатие вложений тянет за собой картиночный слой, а с ним Qt6::Gui. Для
# самой утилиты этого хватает: окон она не открывает, а Gui нужен ради QImage.
add_executable(zametti-store main.cpp)
target_link_libraries(zametti-store PRIVATE zametti-store-lib zametti-code-rules)
if(TARGET zametti-import)
    target_link_libraries(zametti-store PRIVATE zametti-import zametti-imageio)
    target_compile_definitions(zametti-store PRIVATE ZAMETTI_HAVE_IMAGEIO)
endif()
