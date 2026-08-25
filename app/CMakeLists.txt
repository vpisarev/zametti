# Просмотрщик. Единственная цель в проекте, которая знает про Qt.

# Двумя вызовами, а не одним списком: пропажа ЛЮБОЙ части из общего списка
# гасит Qt6_FOUND целиком, и причина «нет Svg» становилась неотличима от «нет
# Qt вовсе» — ровно так первая редакция и соврала в сводке.
find_package(Qt6 QUIET COMPONENTS Gui Widgets)
find_package(Qt6 QUIET COMPONENTS Svg)

# ЕСЛИ ПРОГРАММЫ НЕ БУДЕТ — СКАЗАТЬ ГРОМКО. Раньше здесь стоял STATUS, и в
# потоке чужого вывода его не видно вовсе: настройка проходила «успешно», а
# цели zametti в проекте не появлялось. Владелец наткнулся на это в Xcode —
# искал, почему не собирается libjxl, а на деле не заводился сам просмотрщик,
# и тянуть libjxl было некому.
#
# Причина уезжает в сводку в корне: там её видно последней строкой, а не
# первой из трёхсот.
if(NOT TARGET Qt6::Widgets)
    set(ZAMETTI_APP_SKIPPED "нет Qt6 Gui/Widgets" CACHE INTERNAL "почему нет программы")
    message(NOTICE "Qt6 Widgets не найден — просмотрщика не будет (ядру и его тестам Qt не нужен).")
    return()
endif()

# Svg отдельным пакетом, поэтому и проверка отдельная. Модуль, а не плагин
# imageformats: плагин лежит в необязательном qt6-svg-plugins, и на машине без
# него тулбар оказался бы пустым молча. Слинкованный модуль превращает
# зависимость в объявленную — забыть её при упаковке уже нельзя.
if(NOT TARGET Qt6::Svg)
    set(ZAMETTI_APP_SKIPPED "нет Qt6::Svg" CACHE INTERNAL "почему нет программы")
    message(NOTICE "Qt6 Svg не найден — просмотрщика не будет: нечем рисовать иконки тулбара.")
    if(APPLE)
        message(NOTICE "  brew install qt  (модуль Svg входит в неё; qt@5 не годится)")
    else()
        message(NOTICE "  Поставьте qt6-svg-dev.")
    endif()
    return()
endif()

# Дошли — программа будет.
set(ZAMETTI_APP_SKIPPED "" CACHE INTERNAL "почему нет программы")

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

# СЛОЙ ОКНА — и всё, что здесь есть. Настоящий вид: QTextBrowser, дерево,
# тулбар, панели. Отдельной целью, а не прямо в исполняемом файле: тесту
# редактора нужны те же виджеты, а собирать их дважды незачем.
#
# Позже рядом встанут zametti-ui-android и, может быть, zametti-ui-web. Всё,
# что им пригодится один в один, лежит не здесь, а в ядре.
add_library(zametti-ui STATIC
    find_bar.h
    find_bar.cpp
    history_search.h
    history_search.cpp
    history_panel.h
    history_view.h
    history_controller.h
    archive_view.h
    history_panel.cpp
    history_view.cpp
    history_controller.cpp
    archive_view.cpp
    note_tree.h
    note_tree.cpp
    note_list.h
    note_list.cpp
    note_panels.h
    note_panels.cpp
    search_results.h
    search_results.cpp
    store_search.h
    store_search.cpp
    export_pdf.h
    export_pdf.cpp
    table_object.h
    table_object.cpp
    caret_blink.h
    caret_blink.cpp
    line_field.h
    line_field.cpp
    note_view.h
    note_view.cpp
    editor_widget.h
    editor_widget.cpp
    sync_controller.h
    sync_controller.cpp
    pending_deletes_dialog.h
    pending_deletes_dialog.cpp
    image_insert.h
    image_insert.cpp
    image_viewer.h
    image_viewer.cpp
    icons.h
    icons.cpp
    text_search_target.h
    plain_edit_view.h
    plain_edit_view.cpp
    markdown_edit_view.h
    markdown_edit_view.cpp
    markdown_controller.h
    markdown_controller.cpp
    json_edit_view.h
    json_edit_view.cpp
    settings_controller.h
    settings_controller.cpp
    zapp.h
    zapp.cpp
    lang_editor.h
    lang_editor.cpp
    caption_editor.h
    caption_editor.cpp
    image_importer.h
    image_importer.cpp
    about_window.h
    about_window.cpp
    status_bar.h
    status_bar.cpp
    toolbar.h
    toolbar.cpp
    resources.h
    resources.cpp
    ${CMAKE_SOURCE_DIR}/resources/resources.qrc
)
# Ресурсы (иконка, иконки тулбара, шрифты) собираются сюда, а не в исполняемый
# файл: тестам нужны те же шрифты и те же иконки, а набор, рисующий чужим
# шрифтом, проверял бы не то. Плата — Q_INIT_RESOURCE внутри resources.cpp:
# объектный файл ресурсов в статической библиотеке компоновщик вправе выбросить.
#
# Порог сжатия. Умолчание rcc (--threshold 70) требует, чтобы сжатое было меньше
# исходного на 70%, а ttf ужимается zstd на 48–63% — и восемь начертаний легли
# бы в бинарник как есть. Замер на нашем qrc: без ключа и с --no-compress
# одинаковые 1 723 331 байт, с порогом 30 — 852 558. Ниже тридцати ничего не
# меняется (порог 20 и 5 дают тот же байт), выше сорока начинает расти.
set_property(TARGET zametti-ui APPEND PROPERTY AUTORCC_OPTIONS "--threshold;30")
# build_facts.h порождает CMake рядом со сводкой настройки — см. корневой
# CMakeLists.txt. Каталог общий на все цели: тесту «О программе» нужны те же
# числа, что и окну.
target_include_directories(zametti-ui PUBLIC ${CMAKE_BINARY_DIR}/generated)
target_include_directories(zametti-ui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(zametti-ui PUBLIC zametti-core Qt6::Widgets Qt6::Svg)
target_link_libraries(zametti-ui PRIVATE zametti-code-rules)

# qt_add_executable, а не add_executable: при СТАТИЧЕСКОМ Qt программе нужен
# ещё и плагин платформы (cocoa, xcb, windows), а он такой же статический
# архив, и вписать его в компоновку должен кто-то знающий. Знает Qt — этим
# занимается «доводка» цели, которую qt_add_executable ставит в очередь.
# С обычным Qt разницы никакой: та же add_executable внутри.
#
# Своих плагинов у нас больше нет вовсе: читатели картинок зовутся напрямую
# (см. zametti-core/image/image_read.h), и реестр Qt в этом не участвует.
qt_add_executable(zametti
    main.cpp
)

target_link_libraries(zametti PRIVATE zametti-ui zametti-code-rules)

# Плагины платформы и ввода для статического Qt — см. корневой CMakeLists.
zametti_import_qt_plugins(zametti)

# Иконку в доке оболочка ищет не у окна, а по .desktop-файлу с именем, которое
# приложение сообщает через setDesktopFileName. Без установки этих двух файлов
# док показывает заглушку.
include(GNUInstallDirs)
install(TARGETS zametti RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES ${CMAKE_SOURCE_DIR}/packaging/linux/zametti.desktop
        DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/applications)

# Каталог темы значков называется РАЗМЕРОМ картинки, и размер этот берётся из
# самой картинки, а не пишется числом: иконку перерисовывают, она была 512, а
# стала 1024, и число в пути молча превратилось бы в неправду.
#
# Ширина и высота лежат в заголовке PNG: 8 байт подписи, 4 длины, 4 «IHDR»,
# дальше ширина и высота по 4 байта старшим байтом вперёд. Отсюда смещения 32 и
# 40 в шестнадцатеричной записи (по два знака на байт).
file(READ ${CMAKE_SOURCE_DIR}/resources/zametti.png zametti_png_head HEX LIMIT 24)
string(SUBSTRING "${zametti_png_head}" 32 8 zametti_png_w)
string(SUBSTRING "${zametti_png_head}" 40 8 zametti_png_h)
math(EXPR zametti_png_w "0x${zametti_png_w}")
math(EXPR zametti_png_h "0x${zametti_png_h}")

# Каталог берётся НЕ равным размеру картинки, а самым большим из тех, что тема
# hicolor у себя перечисляет (index.theme, ключ Directories) и который в
# картинку помещается. Каталог 1024x1024 на диске есть, но в перечне его нет, и
# положенное туда оболочка не находит вовсе — то есть «точнее» вышло бы хуже.
# Лишние точки оболочка ужмёт сама, это она умеет.
set(zametti_icon_dir 16)
foreach(size 22 24 32 36 48 64 72 96 128 192 256 512)
    if(NOT zametti_png_w LESS size)
        set(zametti_icon_dir ${size})
    endif()
endforeach()
if(NOT zametti_png_w EQUAL zametti_png_h)
    message(NOTICE "resources/zametti.png не квадратная (${zametti_png_w}×${zametti_png_h}) — "
                   "в доке её растянет")
endif()

install(FILES ${CMAKE_SOURCE_DIR}/resources/zametti.png
        DESTINATION
        ${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/${zametti_icon_dir}x${zametti_icon_dir}/apps)
