# libwebp 1.5.0, вендоринг чужим CMake. Взято из
# https://chromium.googlesource.com/webm/libwebp, тег v1.5.0; чем именно
# обрезано дерево — см. update.sh рядом.
#
# ЗАЧЕМ ОН НАМ. WebP мы только ЧИТАЕМ. Пишем мы ровно один формат — JPEG XL,
# и живой путь ввоза картинки один (app/image_insert.cpp → importImage).
# Но читать надо: в хранилище владельца лежат webp-вложения, оставшиеся от
# разового ввоза чужого дерева, и чужой webp человек вправе принести со
# стороны в любой день.
#
# ПОЧЕМУ ВЕНДОРИНГ, А НЕ ПЛАГИН Qt. Раньше webp читался через libqwebp.so —
# один из штатных плагинов Qt. После того как jxl, tiff, heif и jpeg мы стали
# читать сами, webp остался ЕДИНСТВЕННЫМ форматом, ради которого пришлось бы
# тащить чужой реестр плагинов и зависеть от того, собран ли он в этой сборке
# Qt. Своя библиотека дешевле такой зависимости: на фоне libjxl (9.5 МБ),
# libtiff, highway и microtex декодер webp — крохи.
#
# ТОЛЬКО ЧТЕНИЕ: webpdecoder и webpdemux. Цель webp (с энкодером) и webpmux
# у чужого CMake описаны, но под EXCLUDE_FROM_ALL не собираются никогда —
# писать webp мы не будем нигде.
#
# ДЕМУКСЕР ПОНАДОБИЛСЯ, И ЭТО ВЫЯСНИЛ НАБОР. Сперва я взял один декодер,
# рассудив, что анимация нам не нужна: заметке нужен кадр, а не кино. Но у
# анимированного webp (контейнер VP8X) простой WebPDecode* не читает ВООБЩЕ
# НИЧЕГО — не отдаёт даже первый кадр. В корпусе таких файлов четыре, и Qt их
# читает; молча потерять их значило бы сузить то, что человек может принести.
# Первый кадр достаёт демуксер, и он маленький — один src/demux.
#
# ICC, EXIF и XMP через него всё равно не берём: их достаёт свой обход чанков
# RIFF (zametti-core/image/exif.cpp), и заводить второй путь к тем же байтам
# незачем.
#
# ПОЧЕМУ ЧУЖОЙ CMake, А НЕ СВОЙ СПИСОК ФАЙЛОВ. Та же причина, что у libtiff:
# libwebp сам определяет доступные расширения процессора и раздаёт файлам
# SIMD-диспетчера свои ключи (-msse4.1, -mavx2, -mfpu=neon). Писать это руками
# значит заново делать чужую работу и ошибаться в ней на каждой новой машине.
# Списки исходников он читает из Makefile.am внутри src/ — потому те и
# оставлены в дереве, хотя автотулзами мы не собираем.
set(LIBWEBP_VENDORED_VERSION "1.5.0" PARENT_SCOPE)

# Всё, что не декодер, выключено. Ставим в кэш с FORCE: чужой CMake объявляет
# их через option(), а option() к уже заданной переменной не притрагивается —
# без FORCE значение из прошлой конфигурации пережило бы смену наших правил.
set(WEBP_BUILD_ANIM_UTILS OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_CWEBP      OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_DWEBP      OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_GIF2WEBP   OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_IMG2WEBP   OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_VWEBP      OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_WEBPINFO   OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_WEBPMUX    OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_LIBWEBPMUX OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_EXTRAS     OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_FUZZTEST   OFF CACHE BOOL "" FORCE)

# SIMD — да; потоки декодера — нет. Многопоточность webp нужна тем, кто
# разжимает видеоподобные потоки; у нас разжатие идёт в фоновом потоке ввоза
# целиком, и вторая лесенка потоков внутри библиотеки только мешала бы.
set(WEBP_ENABLE_SIMD ON  CACHE BOOL "" FORCE)
set(WEBP_USE_THREAD  OFF CACHE BOOL "" FORCE)

add_subdirectory(upstream EXCLUDE_FROM_ALL)

# Одна цель наружу: потребителю незачем знать, из скольких объектных
# библиотек чужой CMake складывает свой декодер.
add_library(libwebp INTERFACE)
target_link_libraries(libwebp INTERFACE webpdemux webpdecoder)
target_include_directories(libwebp INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/upstream/src)

# Чужой код чужими правилами: zametti-code-rules ему не цепляется, иначе каждая
# новая версия libwebp ломала бы нам сборку своими предупреждениями.
