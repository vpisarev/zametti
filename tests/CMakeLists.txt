# НАБОРЫ — ОДНИМ ПРОЦЕССОМ.
#
# Было: восемьдесят два исполняемых файла, у каждого свой main, свой блок из
# четырёх строк CMake (скопированный восемьдесят два раза) и своё имя в ctest.
# Стало: один zametti-tests на gtest.
#
# Смысл не в удобстве. Восемьдесят два процесса прятали целый класс бед по
# построению: набор, портящий память или текущий, уносил это с собой в могилу.
# Теперь следующий набор работает в той же памяти — и если предыдущий её
# испортил, это станет видно. Решение владельца.
#
# Обратное осталось: --gtest_filter='Journal.*' гоняет один набор ровно так же,
# как раньше гонялся один бинарник.
#
# КОРПУСА И КАТАЛОГИ ВЫВОДА больше не приходят через argv (в одном процессе он
# на всех один) — их знает TestData (testdata.h). Нет корпуса — набор ГРОМКО
# говорит о пропуске и проходит пустым; молчаливый пропуск неотличим от
# работающей проверки.

find_package(Qt6 QUIET COMPONENTS Core Gui Widgets Test)
if(NOT TARGET Qt6::Widgets)
    message(NOTICE "Qt6 Widgets не найден — наборы собраны не будут.")
    return()
endif()

set(CMAKE_AUTOMOC ON)

# Фаззинг: сколько правок делать в каждой заметке и с каким зерном. Зерно
# задаётся явно, чтобы упавший прогон повторялся дословно.
set(ZAMETTI_FUZZ_ROUNDS 40 CACHE STRING "Сколько случайных правок делать в каждой заметке")
set(ZAMETTI_FUZZ_SEED 20260727 CACHE STRING "Зерно фаззинга операций")
set(ZAMETTI_CORPUS_MUTATIONS 0 CACHE STRING "Сколько мутаций делать из каждого файла")

add_executable(zametti-tests
    main.cpp

    # --- формат ---
    zdocument_test.cpp
    hash_test.cpp
    id_test.cpp
    roundtrip_test.cpp
    table_test.cpp
    golden_test.cpp
    corpus_test.cpp
    math_scan_test.cpp
    math_ir_test.cpp
    zstd_test.cpp

    # --- хранилище и журнал ---
    store_test.cpp
    journal_test.cpp
    history_compress_test.cpp
    fuzz_journal_test.cpp
    archive_test.cpp
    times_test.cpp
    sort_test.cpp
    tree_test.cpp
    note_import_test.cpp

    # --- живая модель документа ---
    reader_test.cpp
    shift_test.cpp
    object_test.cpp
    objects_in_lists_test.cpp
    caption_test.cpp
    caption_click_test.cpp
    znote_test.cpp
    znote_timeline_test.cpp
    docstyle_test.cpp
    zstorage_test.cpp
    table_view_test.cpp
    save_test.cpp
    text_stats_test.cpp
    spaces_test.cpp
    export_test.cpp
    heading_test.cpp
    config_test.cpp
    config_file_test.cpp
    json_highlighter_test.cpp
    ops_test.cpp
    fuzz_ops_test.cpp
    diff_test.cpp
    source_apply_test.cpp
    source_fuzz_test.cpp
    source_caret_test.cpp
    syntax_highlighter_test.cpp
    list_line_test.cpp
    session_test.cpp
    search_test.cpp
    document_roundtrip_test.cpp
    backspace_flat_test.cpp
    tidy_fuzz_test.cpp

    # --- картинки ---
    import_test.cpp
    image_read_test.cpp
    color_test.cpp
    resample_test.cpp
    jxl_encode_test.cpp
    tiff_test.cpp
    import_limits_test.cpp
    exif_test.cpp

    # --- окно ---
    history_search_test.cpp
    history_view_test.cpp
    history_write_test.cpp
    export_pdf_test.cpp
    tree_view_test.cpp
    note_panels_test.cpp
    image_edge_test.cpp
    about_test.cpp
    status_bar_test.cpp
    list_test.cpp
    markdown_edit_test.cpp
    markdown_history_test.cpp
    settings_edit_test.cpp
    editor_test.cpp
    image_insert_test.cpp
    image_shots_test.cpp
    code_edit_test.cpp
    table_shots_test.cpp
    archive_editor_test.cpp
    block_geometry_test.cpp
    caret_matrix_test.cpp
    formula_shots_test.cpp
    inline_formula_test.cpp
    inline_formula_shots_test.cpp
    code_shots_test.cpp
    image_test.cpp
    clipboard_test.cpp
    external_change_test.cpp
    version_stamp_test.cpp
    resources_test.cpp
    toolbar_test.cpp
    live_title_test.cpp
    caret_scale_test.cpp
    zoom_test.cpp
    scroll_test.cpp
)
# md4c здесь не зависимость, а СУДЬЯ: table_test сверяет наш разбор таблиц с
# чужой реализацией того же формата.
target_link_libraries(zametti-tests PRIVATE zametti-ui gtest md4c zstd Qt6::Test
                      zametti-code-rules)
target_include_directories(zametti-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
# Пути к корпусам и к каталогу сборки — константами, а не через argv: в одном
# процессе argv на всех один.
target_compile_definitions(zametti-tests PRIVATE
    ZAMETTI_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    ZAMETTI_BINARY_DIR="${CMAKE_CURRENT_BINARY_DIR}"
    ZAMETTI_FUZZ_ROUNDS=${ZAMETTI_FUZZ_ROUNDS}
    ZAMETTI_FUZZ_SEED=${ZAMETTI_FUZZ_SEED}
    ZAMETTI_CORPUS_MUTATIONS=${ZAMETTI_CORPUS_MUTATIONS}
    ZAMETTI_HAVE_HEIF=$<BOOL:${ZAMETTI_HAVE_HEIF}>)

# ОДИН add_test на всё. Дробить на восемьдесят имён ctest незачем: фильтр gtest
# делает это лучше, а прогон целиком и есть та самая проверка «один процесс».
add_test(NAME zametti-tests COMMAND zametti-tests)

# --- стенды и приборы ------------------------------------------------------
#
# Не наборы: они меряют, а не проверяют, и в общий процесс не идут. Одной
# программой с подкомандой, а не одиннадцатью целями — см. bench_main.cpp.
add_executable(zametti-bench
    bench_main.cpp
    ir_bench.cpp
    history_bench.cpp
    image_bench.cpp
    render_bench.cpp
    quality_study.cpp
    formula_probe.cpp
    formula_sheet_probe.cpp
    cmyk_probe.cpp
    display_scale_probe.cpp
    matrix_probe.cpp
    open_probe.cpp
    zoom_probe.cpp
    undo_probe.cpp
    paste_bench.cpp
    big_bench.cpp
    source_bench.cpp
    inline_formula_probe.cpp
)
# gtest здесь не ради проверок, а ради макросов ZT_EQ/ZT_TRUE: некоторые
# стенды ими же и печатают, что у них сошлось.
target_link_libraries(zametti-bench PRIVATE zametti-ui gtest Qt6::Test zametti-code-rules)
target_include_directories(zametti-bench PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(zametti-bench PRIVATE
    ZAMETTI_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    ZAMETTI_BINARY_DIR="${CMAKE_CURRENT_BINARY_DIR}"
    ZAMETTI_TESTDATA="${CMAKE_SOURCE_DIR}/.testdata")

# Сваливатель markdown в JSON. Отдельная программа, потому что это ИНСТРУМЕНТ:
# им смотрят на разбор чужого файла руками.
add_executable(mddump mddump.cpp)
# gtest — только ради ZT_EQ из общей обвязки; проверок в инструменте нет.
target_link_libraries(mddump PRIVATE zametti-core gtest zametti-code-rules)

# --- механический сторож ---------------------------------------------------
#
# Ядро сознательно зависит от QtGui (там QTextDocument), но виджетов в нём нет
# и быть не должно: всё, что знает про окна, живёт в zametti-ui, а Android
# получит своё. Прежний core-without-qt требовал полной независимости от Qt и
# умер вместе с этим требованием.
#
# Список виджетов сторож спрашивает у самого Qt: первая редакция перечисляла
# имена руками и первым же прогоном объявила виджетом QList.
set(ZAMETTI_WIDGETS_INCLUDE "")
if(TARGET Qt6::Widgets)
    get_target_property(ZAMETTI_WIDGETS_INCLUDE Qt6::Widgets INTERFACE_INCLUDE_DIRECTORIES)
    list(GET ZAMETTI_WIDGETS_INCLUDE 0 ZAMETTI_WIDGETS_INCLUDE)
endif()
add_test(NAME core-without-widgets
         COMMAND ${CMAKE_COMMAND} -DCORE_DIR=${CMAKE_SOURCE_DIR}/zametti-core
                 -DWIDGETS_DIR=${ZAMETTI_WIDGETS_INCLUDE}
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/no_widgets_check.cmake)

# Живой QTextDocument отдаётся наружу только чтобы его показать. Всё остальное
# делается методами заметки — см. правило в CLAUDE.md.
add_test(NAME document-only-for-showing
         COMMAND ${CMAKE_COMMAND}
                 "-DSOURCE_DIRS=${CMAKE_SOURCE_DIR}/zametti-core;${CMAKE_SOURCE_DIR}/app;${CMAKE_SOURCE_DIR}/store;${CMAKE_CURRENT_SOURCE_DIR}"
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/get_document_check.cmake)

# ПАДЕНИЙ НЕ БЫВАЕТ. НИКОГДА (правило владельца в CLAUDE.md).
#
# `zametti --check` — приведение к канону из командной строки. Оно идёт через
# сборку живого документа, а сборщик спрашивает метрики шрифта, то есть
# требует QGuiApplication. Пока в коде стояло обещание «этот режим не требует
# дисплея», приложение падало с core dump на первом же файле.
#
# Юнит-тест этого не поймает: в наборах QApplication есть всегда. Ловить может
# только запуск самой программы, и именно БЕЗ заданной платформы — иначе
# проверялась бы переменная окружения, а не программа. Файл заведомо
# канонический, так что ноль значит «прошло», а не «повезло».
add_test(NAME canon-without-display
         COMMAND zametti --check ${CMAKE_CURRENT_SOURCE_DIR}/канон-без-дисплея.md)
set_tests_properties(canon-without-display PROPERTIES
                     ENVIRONMENT "QT_QPA_PLATFORM=;WAYLAND_DISPLAY=;DISPLAY=")

# При СТАТИЧЕСКОМ Qt исполняемому файлу нужен ещё и плагин платформы, а вписать
# его в компоновку должен кто-то знающий. Знает Qt.
if(Qt6_FOUND AND NOT ZAMETTI_QT_SHARED)
    qt_finalize_target(zametti-tests)
    qt_finalize_target(zametti-bench)
    qt_finalize_target(mddump)
endif()
