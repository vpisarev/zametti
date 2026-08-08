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

        # Вставка картинок: живое окно плюс настоящий конвейер импорта.
        add_executable(image_insert_test image_insert_test.cpp)
        target_link_libraries(image_insert_test PRIVATE zametti-widgets Qt6::Test
                              zametti-warnings)
        target_include_directories(image_insert_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
        add_test(NAME image-insert COMMAND image_insert_test)
        set_tests_properties(image-insert PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

        # Снимки приёмки. Кладёт png в каталог сборки и заодно проверяет то, что
        # поддаётся числу: поворот, вписанность, порядок блоков.
        add_executable(image_shots_test image_shots_test.cpp)
        target_link_libraries(image_shots_test PRIVATE zametti-widgets Qt6::Test
                              zametti-warnings)
        target_include_directories(image_shots_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
        add_test(NAME image-shots COMMAND image_shots_test
                 ${CMAKE_SOURCE_DIR}/.testdata/images/originals
                 ${CMAKE_CURRENT_BINARY_DIR}/снимки-этапа-8)
        set_tests_properties(image-shots PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

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

        # Вшитое в бинарник. Набор нужен именно потому, что на этой машине
        # сломанный qrc неотличим от исправного: IBM Plex стоит в системе, и
        # программа с пустыми ресурсами выглядит нормально. Сломается она у
        # другого человека, и уже в первом окне.
        add_executable(resources_test resources_test.cpp)
        target_link_libraries(resources_test PRIVATE zametti-widgets zametti-warnings)
        target_include_directories(resources_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

        add_test(NAME resources COMMAND resources_test ${CMAKE_CURRENT_BINARY_DIR})
        set_tests_properties(resources PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

        # Тулбар. Кнопка с чужой иконкой, без тултипа или погашенная без
        # объяснения выглядит ровно как исправная — глазами это не ловится.
        add_executable(toolbar_test toolbar_test.cpp)
        target_link_libraries(toolbar_test PRIVATE zametti-widgets zametti-warnings)
        target_include_directories(toolbar_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

        add_test(NAME toolbar COMMAND toolbar_test ${CMAKE_CURRENT_BINARY_DIR})
        set_tests_properties(toolbar PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

        # Состояние сеанса. Поле, не попавшее в запись или в чтение, работает до
        # перезапуска и молча забывается — глазами такое не ловится.
        add_executable(session_test session_test.cpp)
        target_link_libraries(session_test PRIVATE zametti-view zametti-warnings)
        target_include_directories(session_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

        add_test(NAME session COMMAND session_test)
        set_tests_properties(session PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

        # Живой заголовок в средней колонке. Цепочка из трёх звеньев, и молчащее
        # звено посередине глазами не найти: сигнал просто не доходит.
        add_executable(live_title_test live_title_test.cpp)
        target_link_libraries(live_title_test PRIVATE zametti-widgets Qt6::Test
                              zametti-warnings)
        target_include_directories(live_title_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

        add_test(NAME live-title COMMAND live_title_test)
        set_tests_properties(live-title PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

        # Импорт .md: в ту ли папку и видно ли привезённое. Жалоба владельца
        # звучала как одна, а вопросов в ней три, и ответы у них разные.
        add_executable(note_import_test note_import_test.cpp)
        target_link_libraries(note_import_test PRIVATE zametti-widgets zametti-warnings)
        target_include_directories(note_import_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

        add_test(NAME note-import COMMAND note_import_test)
        set_tests_properties(note-import PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
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
    # Стенд подбора параметров: строит кривую «качество против байтов» на
    # большой выборке. Тестом не заводится — это прибор, и гоняется он долго.
    add_executable(quality_study quality_study.cpp)
    target_link_libraries(quality_study PRIVATE zametti-jxl-encode zametti-jxl
                          zametti-imagemeta libjxl Qt6::Gui zametti-warnings)
    target_include_directories(quality_study PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

    # Пробник CMYK: сверяет наш CMS с lcms на профиле 3M Matchprint. Тестом не
    # заводится — ему нужна выборка, приготовленная отдельно; см. заголовок.
    add_executable(cmyk_probe cmyk_probe.cpp)
    target_link_libraries(cmyk_probe PRIVATE zametti-import zametti-imageio libjxl
                          Qt6::Gui zametti-warnings)
    target_include_directories(cmyk_probe PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(import_test import_test.cpp)
    target_link_libraries(import_test PRIVATE zametti-import zametti-imageio Qt6::Gui
                          zametti-warnings)
    target_include_directories(import_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME import COMMAND import_test ${CMAKE_SOURCE_DIR}/.testdata/images/originals)
    set_tests_properties(import PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    add_executable(color_test color_test.cpp)
    # zametti-tiff — ради настоящего CMYK-профиля печати: сочинить его
    # средствами Qt нельзя, а на выдуманном проверка ничего бы не стоила.
    target_link_libraries(color_test PRIVATE zametti-color zametti-jxl-encode zametti-jxl
                          zametti-tiff Qt6::Gui zametti-warnings)
    target_include_directories(color_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME color COMMAND color_test ${CMAKE_SOURCE_DIR}/.testdata/images/originals)
    set_tests_properties(color PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    add_executable(resample_test resample_test.cpp)
    target_link_libraries(resample_test PRIVATE zametti-jxl-encode Qt6::Gui zametti-warnings)
    target_include_directories(resample_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME resample COMMAND resample_test)
    set_tests_properties(resample PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

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
