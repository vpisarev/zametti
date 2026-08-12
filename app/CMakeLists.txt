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

# Модель документа и её преобразования в обе стороны. Виджетов здесь нет
# намеренно: тесты этого слоя гоняются без дисплея и без QtWidgets, и проверяет
# это тест view-without-widgets.
add_library(zametti-view STATIC
    diff.h
    diff.cpp
    diff_view.h
    diff_view.cpp
    doc_model.h
    doc_model.cpp
    document_builder.h
    document_builder.cpp
    document_reader.h
    document_reader.cpp
    document_saver.h
    document_saver.cpp
    edit_history.h
    edit_history.cpp
    editor_ops.h
    editor_ops.cpp
    export_note.h
    export_note.cpp
    marker.h
    marker.cpp
    search.h
    search.cpp
    settings.h
    settings.cpp
    text_stats.h
    text_stats.cpp
)
# История правок живёт в хранилищной библиотеке, а зовёт её редактор — отсюда
# зависимость слоя модели на zametti-store-lib. Обратной нет и быть не должно:
# хранилище про виджеты не знает.
target_link_libraries(zametti-view PUBLIC zametti-core zametti-store-lib Qt6::Gui)
# dtl нужен ровно одному файлу (diff.cpp) и наружу не течёт: заголовочная
# библиотека в публичных зависимостях удорожала бы сборку всем, кто линкует
# zametti-view.
target_link_libraries(zametti-view PRIVATE dtl)
target_link_libraries(zametti-view PRIVATE zametti-warnings)
target_include_directories(zametti-view PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# Слой окна. Отдельной целью, а не прямо в исполняемом файле: тесту редактора
# нужны те же виджеты, а собирать их дважды незачем.
add_library(zametti-widgets STATIC
    find_bar.h
    find_bar.cpp
    history_search.h
    history_search.cpp
    history_panel.h
    history_panel.cpp
    note_tree.h
    note_tree.cpp
    note_list.h
    note_list.cpp
    search_results.h
    search_results.cpp
    store_search.h
    store_search.cpp
    export_pdf.h
    export_pdf.cpp
    note_view.h
    note_view.cpp
    editor_widget.h
    editor_widget.cpp
    image_insert.h
    image_insert.cpp
    icons.h
    icons.cpp
    image_facts.h
    image_facts.cpp
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
set_property(TARGET zametti-widgets APPEND PROPERTY AUTORCC_OPTIONS "--threshold;30")
# Свои читатели картинок (jxl, avif/heic) — статические плагины. Линкуются
# сюда, а не в исполняемый файл, потому что тестам редактора нужны те же
# форматы: набор, который не умеет прочитать jxl, проверял бы не то.
# build_facts.h порождает CMake рядом со сводкой настройки — см. корневой
# CMakeLists.txt. Каталог общий на все цели: тесту «О программе» нужны те же
# числа, что и окну.
target_include_directories(zametti-widgets PUBLIC ${CMAKE_BINARY_DIR}/generated)
target_link_libraries(zametti-widgets PUBLIC zametti-view zametti-imageio Qt6::Widgets Qt6::Svg)
target_link_libraries(zametti-widgets PRIVATE zametti-warnings)

# qt_add_executable, а не add_executable: при СТАТИЧЕСКОМ Qt программе нужен
# ещё и плагин платформы (cocoa, xcb, windows), а он такой же статический
# архив, и вписать его в компоновку должен кто-то знающий. Знает Qt — этим
# занимается «доводка» цели, которую qt_add_executable ставит в очередь.
# С обычным Qt разницы никакой: та же add_executable внутри.
#
# Наши собственные плагины (jxl, heif) в этой доводке не нуждались и не
# нуждаются: qt_add_plugin STATIC заводит рядом с плагином объектный файл с
# Q_IMPORT_PLUGIN и цепляет его как INTERFACE-исходник — он приезжает вместе с
# библиотекой сам.
qt_add_executable(zametti
    main.cpp
)

target_link_libraries(zametti PRIVATE zametti-widgets zametti-store-lib zametti-warnings)

# Иконку в доке оболочка ищет не у окна, а по .desktop-файлу с именем, которое
# приложение сообщает через setDesktopFileName. Без установки этих двух файлов
# док показывает заглушку.
include(GNUInstallDirs)
install(TARGETS zametti RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES ${CMAKE_SOURCE_DIR}/packaging/zametti.desktop
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
