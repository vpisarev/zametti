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
#   store/   плоское хранилище (ZStorage), журнал правок (ZJournal), времена;
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

# Network — под WebDAV-адаптер синхронизации; линкуется PRIVATE, в
# заголовках sync/ сетевых типов нет.
find_package(Qt6 REQUIRED COMPONENTS Core Gui Network)

set(CMAKE_AUTOMOC ON)

add_library(zametti-core STATIC
    # --- формат ---
    format/serializer.h
    format/table.h              format/table.cpp
    format/math_scan.h          format/math_scan.cpp
    format/note_id.h            format/note_id.cpp
    format/hash.h               format/hash.cpp

    # --- хранилище ---
    # ZStorage — один класс, реализация по файлам: заведение, ввоз, проверка,
    # вложения, удаление (store.cpp); ввоз дерева (import_tree.cpp); архив и
    # миграции (archive.cpp); бюро находок (lost_found.cpp); идентичность
    # (store_identity.cpp); пол времени устройства (device_clock.cpp).
    store/zstorage.h            store/zstorage.cpp
    # ZSystem — единственная дверь наружу: удаление и переименование файлов,
    # снос каталога-хранилища, запуск чужих программ. Прямые вызовы за её
    # пределами — таб'у, за этим следит tests/file_ops_check.cmake.
    store/zsystem.h             store/zsystem.cpp
    store/store.cpp
    store/import_tree.cpp
    store/archive.cpp
    store/lost_found.cpp
    store/store_identity.cpp
    store/device_clock.cpp
    store/sync_ops.cpp
    store/sync_ledger.h        store/sync_ledger.cpp
    store/journal.h             store/journal.cpp
    store/times.h               store/times.cpp

    # --- облачная синхронизация: крипто и адаптер (m17) ---
    # Файлов хранилища здесь нет: Keyfile — чистое значение, шифр и адаптер
    # говорят байтами. Встреча с заметками — глаголы ZStorage (sync_ops).
    sync/keyfile.h              sync/keyfile.cpp
    sync/blob_cipher.h          sync/blob_cipher.cpp
    sync/secret_store.h         sync/secret_store.cpp
    sync/secret_bundle.h        sync/secret_bundle.cpp
    sync/bundled_secrets.h      sync/bundled_secrets.cpp
    sync/keyring_secrets.h
    sync/remote_store.h         sync/remote_store.cpp
    sync/folder_remote.h        sync/folder_remote.cpp
    sync/webdav_remote.h        sync/webdav_remote.cpp

    # --- заметка: документ + журнал + всё, что переживает уход и возврат ---
    note/znote.h                note/znote.cpp
    note/note_search.h          note/note_search.cpp
    note/derived.h
    note/caret_spot.h
    note/app_state.h            note/app_state.cpp
    note/zlogs.h                note/zlogs.cpp
    note/znote_timeline.h       note/znote_timeline.cpp

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
    image/deleted_image.h       image/deleted_image.cpp
    image/recompress.h          image/recompress.cpp
    image/image_metadata.h      image/image_metadata.cpp
    image/zimage_cache.h        image/zimage_cache.cpp
    image/heif_handler.h        image/heif_handler.cpp
    image/heif_av1_gav1.h       image/heif_av1_gav1.cpp

    # --- живая модель документа ---
    doc/block_kind.h
    doc/key_binding.h
    doc/note_header.h           doc/note_header.cpp
    doc/document.h              doc/document.cpp
    doc/document_impl.h
    doc/markdown_writer.cpp
    doc/document_diff.cpp        doc/document_edit.cpp
    doc/document_source.cpp
    doc/document_json.cpp
    doc/markdown_reader.cpp
    doc/block_object.h          doc/block_object.cpp
    doc/diff.h                  doc/diff.cpp
    doc/syntax_highlighter.h    doc/syntax_highlighter.cpp
    doc/json_highlighter.h      doc/json_highlighter.cpp
    doc/config_file.h           doc/config_file.cpp
    doc/doc_model.h             doc/doc_model.cpp
    doc/document_builder.h      doc/document_builder.cpp
    doc/document_saver.h        doc/document_saver.cpp
    doc/editor_ops.h            doc/editor_ops.cpp
    doc/export_note.h           doc/export_note.cpp
    doc/marker.h                doc/marker.cpp
    doc/list_line.h             doc/list_line.cpp
    doc/search.h                doc/search.cpp
    doc/settings.h              doc/settings.cpp
    doc/sort_order.h            doc/sort_order.cpp
    doc/formula.h               doc/formula.cpp
    doc/formula_object.h        doc/formula_object.cpp
    doc/object_frame.h          doc/object_frame.cpp
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
    ${CMAKE_CURRENT_SOURCE_DIR}/sync
)

# Sodium — PRIVATE: в заголовках sync/ нет ни одного типа libsodium, наружу
# торчат только QByteArray и QString. У статической библиотеки PRIVATE всё
# равно доедет до компоновки потребителей ($<LINK_ONLY>).
target_link_libraries(zametti-core
    PUBLIC Qt6::Core Qt6::Gui libjxl libtiff
    PRIVATE md4c blake3 zstd jpegli libwebp dtl microtex-all Sodium::Sodium
            Qt6::Network zametti-code-rules)

# Keyring: на Linux — Secret Service по QDBus (ноль зависимостей помимо
# Qt6::DBus, PRIVATE — в заголовке sync/ шины нет); на Windows — Credential
# Manager (Advapi32, тоже ноль нового: она в программе и так есть); на маке —
# Keychain через Security.framework, все секреты одной записью-свёртком
# (bundled_secrets.h; dbus/wincred переедут на свёрток следующими заходами).
#
# Воплощение ровно одно на сборку — файлы взаимоисключающие, и общий у них
# только заголовок keyring_secrets.h.
if(UNIX AND NOT APPLE)
    find_package(Qt6 REQUIRED COMPONENTS DBus)
    target_sources(zametti-core PRIVATE sync/keyring_secrets_dbus.cpp)
    target_link_libraries(zametti-core PRIVATE Qt6::DBus)
elseif(WIN32)
    target_sources(zametti-core PRIVATE sync/keyring_secrets_wincred.cpp)
    target_link_libraries(zametti-core PRIVATE advapi32)
elseif(APPLE)
    target_sources(zametti-core PRIVATE sync/keyring_secrets_keychain.cpp)
    target_link_libraries(zametti-core PRIVATE "-framework Security"
                          "-framework CoreFoundation")
else()
    target_sources(zametti-core PRIVATE sync/keyring_secrets_stub.cpp)
endif()

# HEIF/AVIF — ВЕНДОРЕННЫМИ библиотеками и БЕЗ КЛЮЧА. Обработчик зовётся
# напрямую из image_read, плагином Qt он не является.
#
# Ключ WITH_HEIF был нужен, пока libheif бралась системной: она приводила за
# собой видеокодеки, а с ними — их зависимости, и на маке владелец наступил
# ровно на это. Теперь и контейнер, и оба декодера лежат в 3rdparty, наружу не
# торчит ни одной новой динамической зависимости, и выбирать больше нечего:
# avif и heic читаются всегда, как jxl и webp.
if(NOT TARGET libheif OR NOT TARGET libgav1)
    message(FATAL_ERROR
        "цели libheif/libgav1 не заведены.\n"
        "  add_subdirectory(3rdparty/lib{de265,gav1,heif}) обязаны идти раньше "
        "add_subdirectory(zametti-core) в корневом CMakeLists.txt.")
endif()
target_link_libraries(zametti-core PRIVATE libheif libgav1)

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
