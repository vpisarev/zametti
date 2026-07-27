# Тесты ядра. Линкуются только с zametti-core: если сюда когда-нибудь заедет
# Qt, сборка сломается — это и есть механическая проверка независимости ядра.

add_executable(mddump mddump.cpp)
target_link_libraries(mddump PRIVATE zametti-core zametti-warnings)

add_executable(roundtrip_test roundtrip_test.cpp)
target_link_libraries(roundtrip_test PRIVATE zametti-core zametti-warnings)
add_test(NAME roundtrip COMMAND roundtrip_test)

add_executable(golden_test golden_test.cpp)
target_link_libraries(golden_test PRIVATE zametti-core zametti-warnings)
add_test(NAME golden COMMAND golden_test ${CMAKE_CURRENT_SOURCE_DIR}/golden)

# Фаззинг по корпусу. Пути к деревьям — параметром: содержимое корпусов в
# репозиторий не кладётся, и сборка от чужих каталогов зависеть не должна.
add_executable(corpus_test corpus_test.cpp)
target_link_libraries(corpus_test PRIVATE zametti-core zametti-warnings)

# Каталогов может быть несколько: свои заметки, примеры из спецификации
# CommonMark, примеры из GFM. Их содержимое в репозиторий не кладётся.
set(ZAMETTI_CORPUS "" CACHE STRING "Каталоги с .md для фаззинга инвариантов, через ';'")
# По умолчанию мутаций нет: на самих корпусах инварианты держатся полностью, а
# глубокий фаззинг — отдельный длинный прогон, его запускают руками
# (см. docs/zametti-core-notes.md, раздел про известные ограничения).
set(ZAMETTI_CORPUS_MUTATIONS 0 CACHE STRING "Сколько мутаций делать из каждого файла")
foreach(dir ${ZAMETTI_CORPUS})
    get_filename_component(name "${dir}" NAME)
    get_filename_component(abs "${dir}" ABSOLUTE BASE_DIR ${CMAKE_SOURCE_DIR})
    add_test(NAME corpus-${name} COMMAND corpus_test "${abs}" ${ZAMETTI_CORPUS_MUTATIONS})
endforeach()

# Слой модели документа: сборщик и читатель. Нужен QtGui, но не QtWidgets —
# если цель не собралась (нет Qt), тесты просто не заводятся.
if(TARGET zametti-view)
    add_executable(reader_test reader_test.cpp)
    target_link_libraries(reader_test PRIVATE zametti-view zametti-warnings)
    target_include_directories(reader_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

    add_test(NAME reader COMMAND reader_test)
    set_tests_properties(reader PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    # Те же инварианты по корпусам — каталоги те же, что у corpus_test.
    foreach(dir ${ZAMETTI_CORPUS})
        get_filename_component(name "${dir}" NAME)
        get_filename_component(abs "${dir}" ABSOLUTE BASE_DIR ${CMAKE_SOURCE_DIR})
        add_test(NAME reader-${name} COMMAND reader_test "${abs}")
        set_tests_properties(reader-${name} PROPERTIES ENVIRONMENT
                             "QT_QPA_PLATFORM=offscreen")
    endforeach()

    add_test(NAME view-without-widgets
             COMMAND ${CMAKE_COMMAND} -DBINARY=$<TARGET_FILE:reader_test>
                     -P ${CMAKE_CURRENT_SOURCE_DIR}/no_widgets_check.cmake)
endif()

add_test(NAME core-without-qt
         COMMAND ${CMAKE_COMMAND} -DCORE_DIR=${CMAKE_SOURCE_DIR}/zametti-core
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/no_qt_check.cmake)
