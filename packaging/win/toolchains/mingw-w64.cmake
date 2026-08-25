# Кросс-компиляция под Windows x86-64 через mingw-w64 с Linux-машины.
#
# Правило то же, что у linux-zsys.cmake: собираем НЕ против системы, на которой
# сидим. Здесь это выходит само собой — целевой мир лежит в
# /usr/x86_64-w64-mingw32 и с хостовым /usr/include не пересекается вовсе, —
# но CMAKE_FIND_ROOT_PATH_MODE всё равно прибит: find_library() без него
# спокойно находит хостовые .so и молча ломает сборку на этапе компоновки,
# где сообщение уже не назовёт причины.
#
# Тройка выбирается ключом ZWIN_TRIPLE (по умолчанию x86_64) — на случай
# будущей сборки под arm64 тем же файлом.

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

if(NOT ZWIN_TRIPLE)
    set(ZWIN_TRIPLE x86_64-w64-mingw32)
endif()

# ДРАЙВЕРЫ С СУФФИКСОМ -posix, И ЭТО НЕ ВКУСОВЩИНА.
#
# У mingw-w64 две МОДЕЛИ НИТЕЙ, и Debian ставит обе: «win32» (нити родные, а
# pthread нет вовсе) и «posix» (нити через winpthreads, статически вшиваемые
# ключом -static ниже). Голый x86_64-w64-mingw32-g++ — это win32.
#
# Выбрана posix ради ЧУЖИХ ДЕРЕВЬЕВ. Проверены все вендоренные библиотеки,
# которые вообще упоминают pthread.h: libwebp, libsodium, highway, zstd и
# gtest закрывают его правильным `#if defined(_WIN32)` и под Windows берут
# родные нити, а libgav1 (src/utils/threadpool.cc) закрывает его
# `#if defined(_MSC_VER)` — то есть ветку с CreateThread/WaitForSingleObject
# видит только MSVC, а mingw уходит в pthread_create, которого в модели win32
# нет вовсе. Это ошибка чужого кода, и она чинится в двух файлах одной
# строкой — но 3rdparty/libgav1/update.sh начинает с `rm -rf upstream`, и
# такая правка пропала бы при первом же обновлении МОЛЧА. Чинить в одном
# месте, которое никто не затирает, надёжнее.
#
# Плата названа честно: winpthreads внутри двоичного файла (десятки килобайт) и
# требование собирать Qt ТЕМИ ЖЕ драйверами. Второе — не мелочь: модель нитей
# задаёт, какой libstdc++ подхватится, и смешать в одной программе объекты
# двух моделей значит получить два разных std::mutex под одним именем.
set(CMAKE_C_COMPILER   ${ZWIN_TRIPLE}-gcc-posix)
set(CMAKE_CXX_COMPILER ${ZWIN_TRIPLE}-g++-posix)
set(CMAKE_RC_COMPILER  ${ZWIN_TRIPLE}-windres)
set(CMAKE_AR           ${ZWIN_TRIPLE}-ar CACHE FILEPATH "" FORCE)
set(CMAKE_RANLIB       ${ZWIN_TRIPLE}-ranlib CACHE FILEPATH "" FORCE)

# Корни поиска: мир mingw и наш префикс с собранным (Qt и прочее).
set(CMAKE_FIND_ROOT_PATH /usr/${ZWIN_TRIPLE})
if(DEFINED ENV{ZWPREFIX})
    list(APPEND CMAKE_FIND_ROOT_PATH $ENV{ZWPREFIX})
endif()

# Программы — хостовые (cmake, ninja, perl, python, moc через QT_HOST_PATH);
# всё остальное — только из целевого мира.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# ИЩЕМ ТОЛЬКО СТАТИЧЕСКИЕ АРХИВЫ. У mingw рядом с каждым `libX.a` лежит
# `libX.dll.a` — крошечная import-библиотека, которая не вкладывает код, а лишь
# обещает найти DLL НА ЦЕЛЕВОЙ МАШИНЕ. find_library() по умолчанию видит обе и
# берёт вторую, компоновка проходит, программа собирается — и запускается
# только там, где эта DLL лежит рядом.
#
# Наступили ровно на это: `-fopenmp` (наш ресемплер и перевод цвета,
# zametti-core/image) дал `OpenMP_gomp_LIBRARY = libgomp.dll.a`, и в списке
# зависимостей zametti.exe появилась libgomp-1.dll — единственная не-системная
# из тридцати восьми. Под Linux того же не случилось: libgomp.so есть на любой
# машине, и вопроса не возникало.
#
# Обрезав список суффиксов, мы делаем такую подмену НЕВОЗМОЖНОЙ там, где
# библиотеку ищет find_library().
set(CMAKE_FIND_LIBRARY_SUFFIXES ".a")

# ...но OpenMP ищется НЕ ТАК, и одного суффикса мало. FindOpenMP не зовёт
# find_library вовсе: она разбирает подробную строку компоновки самого драйвера
# и берёт путь, который он назвал для -lgomp, — а это libgomp.dll.a. Поэтому
# archive называем сами, спросив у драйвера прямо. Путь не вписан числом:
# версия gcc и модель нитей в нём меняются, а -print-file-name отвечает про ту
# сборку, которой мы собираем.
execute_process(COMMAND ${CMAKE_C_COMPILER} -print-file-name=libgomp.a
                OUTPUT_VARIABLE ZWIN_LIBGOMP OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_QUIET)
if(ZWIN_LIBGOMP AND EXISTS "${ZWIN_LIBGOMP}")
    set(OpenMP_gomp_LIBRARY "${ZWIN_LIBGOMP}" CACHE FILEPATH "статический libgomp" FORCE)
endif()

# Программа обязана быть ОДНИМ файлом без чужого добра рядом — та же цель, что
# и у переносимой сборки под Linux. У mingw для этого свои три вещи, и все три
# нужны: без -static рядом лягут libgcc_s_seh-1.dll, libstdc++-6.dll и
# libwinpthread-1.dll, а «оно же на моей машине работает» здесь означает лишь
# то, что DLL нашлись в каталоге сборки.
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static -static-libgcc -static-libstdc++")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-static-libgcc -static-libstdc++")

# Windows-заголовки: просим современный API (Win10) и гасим макросы min/max,
# которые ломают std::min/std::max в чужих деревьях. Через *_FLAGS_INIT, а не
# add_compile_definitions(): toolchain читается и внутри try_compile, а команды
# уровня каталога туда не доезжают — проверки Qt и чужих CMake'ов увидели бы
# другой набор определений, чем сама сборка.
#
# WIN32_LEAN_AND_MEAN и UNICODE НЕ задаём глобально нарочно: первый выкидывает
# из windows.h куски, на которые чужие деревья рассчитывают, второй меняет
# смысл целого семейства функций у библиотек, писавшихся под char-API. Qt
# объявляет то и другое себе сам.
set(CMAKE_C_FLAGS_INIT   "-D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 -DNOMINMAX")
set(CMAKE_CXX_FLAGS_INIT "-D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 -DNOMINMAX")

# ЧЕМ ЗАПУСКАТЬ ТО, ЧТО СОБРАЛИ. Кросс-сборка обычно означает «наборы не
# гоняются»: ctest пытается выполнить .exe на Linux и получает Exec format
# error. Wine эту границу снимает, и CMake умеет подставлять его сам — тогда
# `ctest` в каталоге сборки работает как на родной машине.
#
# Это ДЫМОВАЯ проверка, а не приёмка: «зелено под wine» и «зелено под Windows» —
# разные утверждения, и второе проверяется только на живой машине. Но первое
# ловит ровно тот класс бед, ради которого мы и портируем: непереносимый путь,
# текстовый режим файла, отсутствующий плагин платформы.
find_program(ZWIN_WINE wine)
if(ZWIN_WINE)
    set(CMAKE_CROSSCOMPILING_EMULATOR "${ZWIN_WINE}")
else()
    message(STATUS "wine не найден — .exe собираются, но ctest их запустить не сможет")
endif()
