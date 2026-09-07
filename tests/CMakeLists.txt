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

find_package(Qt6 QUIET COMPONENTS Core Gui Widgets Test Network)
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
    fb2_import_test.cpp
    lock_test.cpp
    corpus_test.cpp
    math_scan_test.cpp
    math_ir_test.cpp
    zstd_test.cpp

    # --- хранилище и журнал ---
    store_test.cpp
    store_identity_test.cpp
    bookmarks_test.cpp
    zsystem_test.cpp
    root_note_test.cpp
    journal_test.cpp
    journal_order_test.cpp
    journal_merge_test.cpp
    history_compress_test.cpp
    fuzz_journal_test.cpp
    archive_test.cpp
    times_test.cpp
    sort_test.cpp
    tree_test.cpp
    note_import_test.cpp

    # --- облачная синхронизация ---
    crypto_vectors_test.cpp
    keyfile_test.cpp
    blob_cipher_test.cpp
    secrets_test.cpp
    cloud_store_test.cpp
    webdav_cloud_test.cpp
    sync_push_test.cpp
    sync_engine_test.cpp
    bundled_secrets_test.cpp
    set_cloud_test.cpp
    zstorage_manager_test.cpp
    zstorage_manager_window_test.cpp
    store_manager_dialog_test.cpp
    store_live_test.cpp
    sync_fuzz_test.cpp
    zlogs_test.cpp

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
    file_canon_test.cpp
    heal_test.cpp
    text_stats_test.cpp
    spaces_test.cpp
    export_test.cpp
    heading_test.cpp
    config_test.cpp
    theme_test.cpp
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
    av1_edges_test.cpp
    color_test.cpp
    resample_test.cpp
    jxl_encode_test.cpp
    tiff_test.cpp
    import_limits_test.cpp
    exif_test.cpp
    deleted_image_test.cpp

    # --- окно ---
    zbook_view_test.cpp
    toc_list_test.cpp
    history_search_test.cpp
    history_view_test.cpp
    history_write_test.cpp
    export_pdf_test.cpp
    tree_view_test.cpp
    note_panels_test.cpp
    note_commands_test.cpp
    switch_clean_test.cpp
    open_storage_test.cpp
    info_folder_test.cpp
    object_keys_test.cpp
    image_edge_test.cpp
    about_test.cpp
    status_bar_test.cpp
    list_test.cpp
    markdown_edit_test.cpp
    markdown_history_test.cpp
    settings_edit_test.cpp
    editor_test.cpp
    type_rules_test.cpp
    image_insert_test.cpp
    image_viewer_test.cpp
    formula_ink_test.cpp
    caret_quiet_test.cpp
    content_column_test.cpp
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
    ui_style_test.cpp
    toolbar_state_test.cpp
    live_title_test.cpp
    caret_scale_test.cpp
    caret_paint_test.cpp
    font_dpi_test.cpp
    zoom_test.cpp
    scroll_test.cpp
)
# md4c здесь не зависимость, а СУДЬЯ: table_test сверяет наш разбор таблиц с
# чужой реализацией того же формата.
# Sodium здесь не только зависимость классов синка, но и СУДЬЯ:
# crypto_vectors_test зовёт примитивы напрямую и сверяет с байтами RFC.
# Qt6::Network — обвязке WebDAV (проба порта) и самому адаптеру.
target_link_libraries(zametti-tests PRIVATE zametti-ui gtest md4c zstd Qt6::Test
                      Sodium::Sodium Qt6::Network zametti-code-rules)
# Плагины платформы для статического Qt — см. корневой CMakeLists. Наборам
# offscreen НУЖЕН БОЛЬШЕ ВСЕХ: main.cpp ставит его сам, когда платформа не
# задана снаружи. Без этой строки в статической сборке весь набор падал с
# SIGABRT на QGuiApplication — «Available platform plugins are: xcb», — то
# есть приёмки у переносимой сборки не было вовсе.
zametti_import_qt_plugins(zametti-tests)
zametti_windows_manifest(zametti-tests)
target_include_directories(zametti-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
# Пути к корпусам и к каталогу сборки — константами, а не через argv: в одном
# процессе argv на всех один.
target_compile_definitions(zametti-tests PRIVATE
    ZAMETTI_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    ZAMETTI_BINARY_DIR="${CMAKE_CURRENT_BINARY_DIR}"
    ZAMETTI_FUZZ_ROUNDS=${ZAMETTI_FUZZ_ROUNDS}
    ZAMETTI_FUZZ_SEED=${ZAMETTI_FUZZ_SEED}
    ZAMETTI_CORPUS_MUTATIONS=${ZAMETTI_CORPUS_MUTATIONS})

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
    md4c_spaces_probe.cpp
    ui_metrics_probe.cpp
    undo_probe.cpp
    paste_bench.cpp
    big_bench.cpp
    switch_bench.cpp
    write_probe.cpp
    setdoc_probe.cpp
    source_bench.cpp
    inline_formula_probe.cpp
    argon2_probe.cpp
    keyring_probe.cpp
    webdav_probe.cpp
)
# gtest здесь не ради проверок, а ради макросов ZT_EQ/ZT_TRUE: некоторые
# стенды ими же и печатают, что у них сошлось.
target_link_libraries(zametti-bench PRIVATE zametti-ui gtest Qt6::Test Sodium::Sodium
                      zametti-code-rules)
# Пробник keyring на маке меряет связку сырыми SecItem* (вопрос §1.2 матрицы) —
# рамка нужна самому бенчу, не только ядру.
if(APPLE)
    target_link_libraries(zametti-bench PRIVATE "-framework Security"
                          "-framework CoreFoundation")
endif()
# На Linux тот же пробник меряет Secret Service сырыми QDBus-вызовами
# (замер «атрибуты у запертой», посадка legacy-записей для миграции).
if(UNIX AND NOT APPLE)
    find_package(Qt6 REQUIRED COMPONENTS DBus)
    target_link_libraries(zametti-bench PRIVATE Qt6::DBus)
endif()
zametti_import_qt_plugins(zametti-bench)
zametti_windows_manifest(zametti-bench)
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
zametti_import_qt_plugins(mddump)
zametti_windows_manifest(mddump)

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
                 "-DSOURCE_DIRS=${CMAKE_SOURCE_DIR}/zametti-core;${CMAKE_SOURCE_DIR}/app;${CMAKE_CURRENT_SOURCE_DIR}"
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/get_document_check.cmake)

# Окна сообщений строятся в одном месте (ZApp::messageBox / warn / inform /
# ask) — иначе они выглядят по-разному (правило владельца в CLAUDE.md).
add_test(NAME message-boxes-through-zapp
         COMMAND ${CMAKE_COMMAND}
                 "-DSOURCE_DIRS=${CMAKE_SOURCE_DIR}/app;${CMAKE_CURRENT_SOURCE_DIR}"
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/message_box_check.cmake)

# ТАБ'У: разрушительное — только через ZSystem (правило владельца, первые
# строки CLAUDE.md). Наборы и пробники проверяются НАРАВНЕ с программой: каталог
# владельца снёс именно пробник, и сторож, обходящий tests/, обходил бы место
# происшествия.
add_test(NAME file-ops-only-through-guard
         COMMAND ${CMAKE_COMMAND}
                 "-DSOURCE_DIRS=${CMAKE_SOURCE_DIR}/zametti-core;${CMAKE_SOURCE_DIR}/app;${CMAKE_SOURCE_DIR}/tools;${CMAKE_CURRENT_SOURCE_DIR}"
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/file_ops_check.cmake)

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

# ОКНО БЕЗ ХРАНИЛИЩА ПОДНИМАЕТСЯ, А НЕ ОТКАЗЫВАЕТСЯ РАБОТАТЬ.
#
# Раньше программа без хранилища печатала подсказку в терминал и выходила с
# кодом 2; теперь она открывает пустое окно, в котором горит одна кнопка —
# «открыть или завести хранилище». Юнит-тесты этого не видят: окно они целиком
# не собирают, а весь смысл здесь в том, ЧТО ДЕЛАЕТ ПРОГРАММА, когда открывать
# нечего.
#
# XDG_CONFIG_HOME уводится в каталог сборки, и это половина проверки: без него
# нашёлся бы state.json владельца с его хранилищем, и набор проверял бы совсем
# другой случай.
#
# Платформа здесь задаётся ЯВНО (offscreen), а не гасится, как у
# canon-without-display: тому режиму дисплей не нужен по существу, а окну нужен
# — обычному запуску платформу выбирает Qt, и навязывать offscreen внутри
# программы нельзя, иначе человек остался бы без окна. Дисплей при этом гасим,
# чтобы окно не выскакивало перед владельцем во время прогона наборов.
add_test(NAME window-without-store
         COMMAND zametti)
set_tests_properties(window-without-store PROPERTIES
                     ENVIRONMENT "QT_QPA_PLATFORM=offscreen;WAYLAND_DISPLAY=;DISPLAY=;XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/no-store-config;ZAMETTI_PROBE_QUIT_MS=1500")

# КОМАНДНЫЙ ВИД ХРАНИЛИЩА — `zametti store …`.
#
# Юнит-тесты зовут ZStorage напрямую и потому не видят ни диспетчера
# подкоманды, ни разбора ключей, ни кодов возврата. До слияния программ
# (26.08.2026) командный режим не был покрыт ни одним тестом-процессом вовсе;
# теперь он живёт внутри самой программы, и молчаливая пропажа задела бы её.
#
# Окружение — как у canon-without-display: платформу и дисплей гасим, чтобы
# проверялась сама программа, а не переменная среды. XDG_CONFIG_HOME уводится
# в каталог сборки: наборы не читают конфиг владельца и не пишут в него.
add_test(NAME store-subcommand
         COMMAND ${CMAKE_COMMAND} -DZAMETTI=$<TARGET_FILE:zametti>
                 -DWORK=${CMAKE_CURRENT_BINARY_DIR}/store-cli
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/store_cli_check.cmake)
set_tests_properties(store-subcommand PROPERTIES
                     ENVIRONMENT "QT_QPA_PLATFORM=;WAYLAND_DISPLAY=;DISPLAY=;XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/store-cli-config")

# При СТАТИЧЕСКОМ Qt исполняемому файлу нужен ещё и плагин платформы, а вписать
# его в компоновку должен кто-то знающий. Знает Qt.
if(Qt6_FOUND AND NOT ZAMETTI_QT_SHARED)
    qt_finalize_target(zametti-tests)
    qt_finalize_target(zametti-bench)
    qt_finalize_target(mddump)
endif()
