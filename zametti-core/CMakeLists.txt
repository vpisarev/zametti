# ЯДРО. Одна цель на всё, что не виджеты.
#
# Раньше здесь было пять библиотек — zametti-core, zametti-store-lib,
# zametti-view и семь целей imageio, — и граница между ними была историей их
# появления, а не смыслом. Решение владельца: ядро сознательно большое.
# Отдельная image-io потеряла смысл, когда ядро всё равно стало зависеть от
# QtGui, а сюда же придёт облачная синхронизация. Довод — удобство, а не
# чистота графа зависимостей: ядро Linux немаленькое и победило микроядра.
#
# ЧТО ВНУТРИ, по подкаталогам:
#
#   format/  разбор и запись markdown, отпечатки, id, таблицы, математика.
#            Единственная часть, которой Qt не нужен вовсе;
#   store/   плоское хранилище, журнал правок, архив, бюро находок, времена;
#   image/   чтение и запись картинок: jxl, jpeg, tiff, webp, heif, цвет,
#            ресемплер, конвейер ввоза;
#   doc/     живая модель документа: сборщик, читатель, сохранятель, операции
#            правки, разность, счёт слов, поиск, вёрстка таблиц и формул.
#
# ПОЧЕМУ doc/ ЗДЕСЬ, А НЕ В UI. Имя прежней цели (zametti-view) обманывало:
# ни один её файл не берёт ни одного виджетного типа. Граница проведена по
# вопросу «что переиспользует будущий zametti-ui-android»: разбор, запись,
# журнал, поиск, счёт, операции правки, слой объектов, геометрию таблиц и
# вёрстку формул — всё; NoteEditor с keyPressEvent и QTextBrowser — ничего.
#
# ЖЁСТКОЕ ТРЕБОВАНИЕ ОСТАЛОСЬ ОДНО: ни одного include из QtWidgets. Ядро
# зависит от QtGui сознательно (QTextDocument живёт там), но про окна не знает.
# Стережёт это набор core-without-widgets.

find_package(Qt6 REQUIRED COMPONENTS Core Gui)

set(CMAKE_AUTOMOC ON)

add_library(zametti-core STATIC
    # --- формат ---
    format/serializer.h
    format/table.h              format/table.cpp
    format/math_scan.h          format/math_scan.cpp
    format/note_id.h            format/note_id.cpp
    format/hash.h               format/hash.cpp

    # --- хранилище ---
    store/store.h               store/store.cpp
    store/journal.h             store/journal.cpp
    store/history_rules.h       store/history_rules.cpp
    store/times.h               store/times.cpp
    store/archive.h             store/archive.cpp
    store/lost_found.h          store/lost_found.cpp
    store/znote_history.h       store/znote_history.cpp

    # --- заметка: документ + журнал + всё, что переживает уход и возврат ---
    note/znote.h                note/znote.cpp

    # --- картинки ---
    image/exif.h                image/exif.cpp
    image/import_limits.h       image/import_limits.cpp
    image/color.h               image/color.cpp
    image/image_read.h          image/image_read.cpp
    image/jpeg_read.h           image/jpeg_read.cpp
    image/jxl_read.h            image/jxl_read.cpp
    image/webp_read.h           image/webp_read.cpp
    image/tiff_reader.h         image/tiff_reader.cpp
    image/jxl_encoder.h         image/jxl_encoder.cpp
    image/resample.h            image/resample.cpp
    image/import.h              image/import.cpp
    image/ladder.h              image/ladder.cpp
    image/recompress.h          image/recompress.cpp

    # --- живая модель документа ---
    doc/block_kind.h
    doc/note_header.h           doc/note_header.cpp
    doc/document.h              doc/document.cpp
    doc/document_impl.h
    doc/markdown_writer.cpp
    doc/document_diff.cpp        doc/document_edit.cpp
    doc/document_json.cpp
    doc/markdown_reader.cpp
    doc/block_object.h          doc/block_object.cpp
    doc/diff.h                  doc/diff.cpp
    doc/diff_view.h             doc/diff_view.cpp
    doc/doc_model.h             doc/doc_model.cpp
    doc/document_builder.h      doc/document_builder.cpp
    doc/document_saver.h        doc/document_saver.cpp
    doc/editor_ops.h            doc/editor_ops.cpp
    doc/export_note.h           doc/export_note.cpp
    doc/marker.h                doc/marker.cpp
    doc/search.h                doc/search.cpp
    doc/settings.h              doc/settings.cpp
    doc/sort_order.h            doc/sort_order.cpp
    doc/formula.h               doc/formula.cpp
    doc/table_view.h            doc/table_view.cpp
    doc/text_stats.h            doc/text_stats.cpp
)

# Четыре каталога — четыре пути поиска, и все ПУБЛИЧНЫЕ. Так `#include "document.h"`
# работает и внутри ядра, и снаружи, а раскладка по подкаталогам остаётся
# удобством чтения, а не новой границей.
target_include_directories(zametti-core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/format
    ${CMAKE_CURRENT_SOURCE_DIR}/store
    ${CMAKE_CURRENT_SOURCE_DIR}/image
    ${CMAKE_CURRENT_SOURCE_DIR}/doc
    ${CMAKE_CURRENT_SOURCE_DIR}/note
)

target_link_libraries(zametti-core
    PUBLIC Qt6::Core Qt6::Gui libjxl libtiff
    PRIVATE md4c blake3 zstd jpegli libwebp dtl microtex-all zametti-code-rules)

# HEIF/AVIF — по ключу, системной библиотекой. Обработчик зовётся напрямую из
# image_read, плагином он больше не является.
if(TARGET zametti-heif)
    target_link_libraries(zametti-core PRIVATE zametti-heif)
    target_compile_definitions(zametti-core PRIVATE ZAMETTI_HAVE_HEIF)
endif()

# Ресемплер и перевод цвета раскладываются по строкам без связей между ними —
# параллелятся даром. OpenMP берём, ТОЛЬКО если компилятор его даёт: на macOS с
# AppleClang он требует отдельной libomp, а зависеть от неё нельзя (правило
# владельца — компилятор, CMake и Qt, и ничего больше).
find_package(OpenMP QUIET COMPONENTS CXX)
if(OpenMP_CXX_FOUND)
    target_link_libraries(zametti-core PRIVATE OpenMP::OpenMP_CXX)
    set(ZAMETTI_HAVE_OPENMP ON PARENT_SCOPE)
else()
    set(ZAMETTI_HAVE_OPENMP OFF PARENT_SCOPE)
endif()

set_target_properties(zametti-core PROPERTIES POSITION_INDEPENDENT_CODE ON)
