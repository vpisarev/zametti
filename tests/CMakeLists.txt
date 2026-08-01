# Тесты ядра. Линкуются только с zametti-core: если сюда когда-нибудь заедет
# Qt, сборка сломается — это и есть механическая проверка независимости ядра.

add_executable(mddump mddump.cpp)
target_link_libraries(mddump PRIVATE zametti-core zametti-warnings)

# Пробник представления IR: аллокации, время, память. Тестом не заводится —
# это прибор, его запускают руками (см. docs/zametti-m5-report.md).
add_executable(ir_bench ir_bench.cpp)
target_link_libraries(ir_bench PRIVATE zametti-core zametti-warnings)

add_executable(zstd_test zstd_test.cpp)
target_link_libraries(zstd_test PRIVATE zstd zametti-warnings)
target_include_directories(zstd_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME zstd COMMAND zstd_test)

add_executable(hash_test hash_test.cpp)
target_link_libraries(hash_test PRIVATE zametti-core zametti-warnings)
target_include_directories(hash_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME blake3 COMMAND hash_test)

add_executable(id_test id_test.cpp)
target_link_libraries(id_test PRIVATE zametti-core zametti-warnings)
target_include_directories(id_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME note-id COMMAND id_test)

add_executable(store_test store_test.cpp)
target_link_libraries(store_test PRIVATE zametti-store-lib zametti-warnings)
target_include_directories(store_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME store COMMAND store_test)

add_executable(journal_test journal_test.cpp)
target_link_libraries(journal_test PRIVATE zametti-store-lib zametti-warnings)
target_include_directories(journal_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME journal COMMAND journal_test)

add_executable(fuzz_journal_test fuzz_journal_test.cpp)
target_link_libraries(fuzz_journal_test PRIVATE zametti-store-lib zametti-warnings)
target_include_directories(fuzz_journal_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME fuzz-journal COMMAND fuzz_journal_test)

add_executable(roundtrip_test roundtrip_test.cpp)
target_link_libraries(roundtrip_test PRIVATE zametti-core zametti-warnings)
add_test(NAME roundtrip COMMAND roundtrip_test)

# Арена IR: границы обоих координатных пространств, перенос между документами,
# независимость копии, резерв без реаллокаций.
add_executable(arena_test arena_test.cpp)
target_link_libraries(arena_test PRIVATE zametti-core zametti-warnings)
target_include_directories(arena_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME arena COMMAND arena_test)

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

        add_executable(search_test search_test.cpp)
        target_link_libraries(search_test PRIVATE zametti-widgets Qt6::Test zametti-warnings)
        target_include_directories(search_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
        add_test(NAME search COMMAND search_test)
        set_tests_properties(search PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

        add_executable(tree_test tree_test.cpp)
        target_link_libraries(tree_test PRIVATE zametti-widgets Qt6::Test zametti-warnings)
        target_include_directories(tree_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
        add_test(NAME tree COMMAND tree_test)
        set_tests_properties(tree PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

        add_executable(image_test image_test.cpp)
        target_link_libraries(image_test PRIVATE zametti-widgets Qt6::Test zametti-warnings)
        target_include_directories(image_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

        add_test(NAME images COMMAND image_test)
        set_tests_properties(images PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

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

# Стенд отрисовки. Цель собирается, но add_test для него НЕТ и не будет: это
# замер, а не проверка, и смотрят на него тогда, когда есть подозрение, что
# правка замедлила отрисовку. Запускать руками, см. заголовок файла.
add_executable(render_bench render_bench.cpp)
target_link_libraries(render_bench PRIVATE zametti-widgets Qt6::Test zametti-warnings)
target_include_directories(render_bench PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

if(TARGET zametti-jxl-encode)
    add_executable(jxl_encode_test jxl_encode_test.cpp)
    target_link_libraries(jxl_encode_test PRIVATE zametti-jxl-encode zametti-jxl
                          zametti-imagemeta Qt6::Gui zametti-warnings)
    target_include_directories(jxl_encode_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME jxl-encode
             COMMAND jxl_encode_test ${CMAKE_SOURCE_DIR}/.testdata/images/originals)
    set_tests_properties(jxl-encode PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()

if(TARGET zametti-tiff)
    add_executable(tiff_test tiff_test.cpp)
    target_link_libraries(tiff_test PRIVATE zametti-tiff Qt6::Gui zametti-warnings)
    target_include_directories(tiff_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME tiff COMMAND tiff_test ${CMAKE_SOURCE_DIR}/.testdata/images/originals)
    set_tests_properties(tiff PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()

add_executable(import_limits_test import_limits_test.cpp)
target_link_libraries(import_limits_test PRIVATE zametti-imagemeta zametti-warnings)
target_include_directories(import_limits_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME import-limits COMMAND import_limits_test)

add_executable(exif_test exif_test.cpp)
target_link_libraries(exif_test PRIVATE zametti-imagemeta zametti-warnings)
target_include_directories(exif_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME exif COMMAND exif_test ${CMAKE_SOURCE_DIR}/.testdata/images/originals)

# Стенд кодеков этапа 8. Тоже замер, а не проверка: add_test для него нет.
# Запускать под taskset, см. заголовок файла.
add_executable(image_bench image_bench.cpp)
target_link_libraries(image_bench PRIVATE libjxl zametti-imageio Qt6::Gui zametti-warnings)

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

add_executable(tidy_fuzz_test tidy_fuzz_test.cpp)
target_link_libraries(tidy_fuzz_test PRIVATE zametti-widgets Qt6::Test zametti-warnings)
target_include_directories(tidy_fuzz_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME tidy-fuzz COMMAND tidy_fuzz_test)
set_tests_properties(tidy-fuzz PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
