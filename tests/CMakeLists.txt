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
# Фаззинг операций: сколько правок делать в каждой заметке и с каким зерном.
# Зерно задаётся явно, чтобы упавший прогон повторялся дословно.
set(ZAMETTI_FUZZ_ROUNDS 40 CACHE STRING "Сколько случайных правок делать в каждой заметке")
set(ZAMETTI_FUZZ_SEED 20260727 CACHE STRING "Зерно фаззинга операций")
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

    add_executable(save_test save_test.cpp)
    target_link_libraries(save_test PRIVATE zametti-view zametti-warnings)
    target_include_directories(save_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

    add_test(NAME save COMMAND save_test ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(save PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    add_executable(ops_test ops_test.cpp)
    target_link_libraries(ops_test PRIVATE zametti-view zametti-warnings)
    target_include_directories(ops_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

    add_test(NAME ops COMMAND ops_test)
    set_tests_properties(ops PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    # Списки правят чаще всего остального, и ошибка здесь дороже любой другой:
    # отдельный набор, проверяющий их живым набором в настоящем редакторе.
    add_executable(list_test list_test.cpp)
    target_link_libraries(list_test PRIVATE zametti-widgets Qt6::Test zametti-warnings)
    target_include_directories(list_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME lists COMMAND list_test)
    set_tests_properties(lists PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    # Фаззинг операций. Каталоги с заметками — те же, что и у фаззинга ядра:
    # содержимое в репозиторий не кладётся, и без них тест просто не заводится.
    add_executable(fuzz_ops_test fuzz_ops_test.cpp)
    target_link_libraries(fuzz_ops_test PRIVATE zametti-view zametti-warnings)
    target_include_directories(fuzz_ops_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    foreach(dir ${ZAMETTI_CORPUS})
        get_filename_component(name "${dir}" NAME)
        get_filename_component(abs "${dir}" ABSOLUTE BASE_DIR ${CMAKE_SOURCE_DIR})
        add_test(NAME fuzz-ops-${name}
                 COMMAND fuzz_ops_test "${abs}" ${ZAMETTI_FUZZ_ROUNDS} ${ZAMETTI_FUZZ_SEED})
        set_tests_properties(fuzz-ops-${name}
                             PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    endforeach()

    add_executable(undo_test undo_test.cpp)
    target_link_libraries(undo_test PRIVATE zametti-view zametti-warnings)
    target_include_directories(undo_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

    add_test(NAME undo COMMAND undo_test)
    set_tests_properties(undo PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    # Единственный тест с QtWidgets: обе ошибки, ради которых он написан, живут
    # в виджете и над голым QTextDocument не ловятся. Слой модели от виджетов
    # по-прежнему свободен — за этим следит view-without-widgets ниже.
    find_package(Qt6 QUIET COMPONENTS Test)
    if(Qt6Test_FOUND AND TARGET zametti-widgets)
        add_executable(editor_test editor_test.cpp)
        target_link_libraries(editor_test PRIVATE zametti-widgets Qt6::Test zametti-warnings)
        target_include_directories(editor_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

        add_test(NAME editor COMMAND editor_test ${CMAKE_CURRENT_BINARY_DIR})
        set_tests_properties(editor PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

        add_executable(clipboard_test clipboard_test.cpp)
        target_link_libraries(clipboard_test PRIVATE zametti-widgets Qt6::Test zametti-warnings)
        target_include_directories(clipboard_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

        add_test(NAME clipboard COMMAND clipboard_test ${CMAKE_CURRENT_BINARY_DIR})
        set_tests_properties(clipboard PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

        add_executable(external_change_test external_change_test.cpp)
        target_link_libraries(external_change_test
                              PRIVATE zametti-widgets Qt6::Test zametti-warnings)
        target_include_directories(external_change_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

        add_test(NAME external-change
                 COMMAND external_change_test ${CMAKE_CURRENT_BINARY_DIR})
        set_tests_properties(external-change PROPERTIES ENVIRONMENT
                             "QT_QPA_PLATFORM=offscreen")
    endif()

    add_test(NAME view-without-widgets
             COMMAND ${CMAKE_COMMAND} -DBINARY=$<TARGET_FILE:reader_test>
                     -P ${CMAKE_CURRENT_SOURCE_DIR}/no_widgets_check.cmake)
endif()

add_test(NAME core-without-qt
         COMMAND ${CMAKE_COMMAND} -DCORE_DIR=${CMAKE_SOURCE_DIR}/zametti-core
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/no_qt_check.cmake)

add_executable(caret_scale_test caret_scale_test.cpp)
target_link_libraries(caret_scale_test PRIVATE zametti-widgets Qt6::Test zametti-warnings)
target_include_directories(caret_scale_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME caret-scale COMMAND caret_scale_test)
set_tests_properties(caret-scale PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(backspace_flat_test backspace_flat_test.cpp)
target_link_libraries(backspace_flat_test PRIVATE zametti-widgets Qt6::Test zametti-warnings)
target_include_directories(backspace_flat_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME backspace-flat COMMAND backspace_flat_test)
set_tests_properties(backspace-flat PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
