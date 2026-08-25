# Toolchain переносимой сборки под Linux: всё собирается против sysroot
# Ubuntu 20.04 (glibc 2.31), а не против системы, на которой мы сидим.
#
#   source ~/work/zsys/bin/zenv.sh
#   cmake -S . -B build-portable \
#         -DCMAKE_TOOLCHAIN_FILE=packaging/toolchains/linux-zsys.cmake \
#         -DCMAKE_PREFIX_PATH=$ZPREFIX -DWITH_STATIC_QT=ON
#
# Этим же файлом собраны Qt и OpenSSL, лежащие в $ZPREFIX. Так и задумано:
# разошедшиеся ключи у Qt и у программы — это бинарь, который собрался, но не
# запускается, и разбираться в таком потом дороже всего.
#
# Машинно-специфичного внутри НЕТ: пути приходят из окружения (zenv.sh), а
# рядом со временем лягут toolchain'ы для windows и macos.

if(NOT DEFINED ENV{ZSYS})
    message(FATAL_ERROR
        "Не задан ZSYS — путь к sysroot.\n"
        "  source ~/work/zsys/bin/zenv.sh")
endif()
if(NOT IS_DIRECTORY "$ENV{ZSYS}/usr/include")
    message(FATAL_ERROR "ZSYS=$ENV{ZSYS} не похож на sysroot: нет usr/include")
endif()

# CMAKE_SYSTEM_NAME НЕ ЗАДАЁМ НАРОЧНО. Архитектура та же, сборка остаётся
# «родной», и Qt не требует QT_HOST_PATH с отдельным хостовым Qt. Объявить
# кросс-сборку значило бы завести себе вторую сборку Qt на пустом месте.

set(CMAKE_SYSROOT "$ENV{ZSYS}")
set(CMAKE_C_COMPILER "$ENV{ZSYS}/bin/gcc")
set(CMAKE_CXX_COMPILER "$ENV{ZSYS}/bin/g++")

# ГЛАВНОЕ. Без этого find_library() спокойно берёт /usr/lib/x86_64-linux-gnu
# ХОСТА, сборка проходит, а переносимость утекает молча — ровно так же, как
# она утекала через абсолютные симлинки внутри самого sysroot.
set(CMAKE_FIND_ROOT_PATH "$ENV{ZSYS}" "$ENV{ZPREFIX}")
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
# А программы (perl, python, ninja, wayland-scanner, moc) нужны ХОСТОВЫЕ: они
# исполняются здесь и сейчас, а не на целевой машине.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)

# ОБЯЗАТЕЛЬНЫ, а не желательны: libstdc++ у gcc-15 требует GLIBCXX_3.4.32,
# которого нет даже в той Ubuntu 20.04, откуда снят sysroot. Без этих ключей
# собранное не запускается на целевой системе — замерено.
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static-libstdc++ -static-libgcc")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-static-libstdc++ -static-libgcc")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "-static-libstdc++ -static-libgcc")

# СЕМЕЙСТВО xcb-util И SM/ICE — СТАТИЧЕСКИ.
#
# Шесть библиотек, которые тянет xcb-плагин Qt (xcb-cursor, xcb-icccm,
# xcb-image, xcb-keysyms, xcb-render-util, xcb-util), лежат в пакетах
# priority=EXTRA — самый низкий: в базовую установку они не входят никогда и
# оказываются на машине, только если их притянул кто-то другой. libxcb-cursor0
# — та самая, из-за которой Qt 6.5+ падает с «could not load the Qt platform
# plugin xcb»; это самая частая жалоба на развёртывание Qt-программ.
#
# Все шесть есть в sysroot архивами, и они тонкие обёртки над libxcb, так что
# вшить их дешевле, чем объяснять человеку, какие шесть пакетов доставить.
#
# libSM и libICE (сессия X11) ЗДЕСЬ НЕТ НАРОЧНО, хотя архивы их лежат рядом и
# соблазн был. Статическая libICE.a требует arc4random_buf из libbsd, а libbsd
# архивом в sysroot не поставляется вовсе — вшив ICE, мы обменяли бы две
# библиотеки priority=optional на одну такую же плюс возню. Обе остаются
# динамическими: на любом рабочем столе с X11 они есть.
#
# ЗАДАВАТЬ НАДО ЗДЕСЬ, А НЕ ТОЛЬКО ПРИ СБОРКЕ Qt. Модуль FindXCB.cmake
# устанавливается ВМЕСТЕ с Qt и переискивает библиотеки заново у каждого
# потребителя — то есть решает кэш нашей сборки, а не кэш сборки Qt. Первая
# попытка задала их только у Qt, и из тридцати семи зависимостей ушла ровно
# одна.
# ВСЕ ШЕСТЬ БЕРУТСЯ ИЗ $ZPREFIX, а не из sysroot, и это решение с двумя
# замеренными причинами.
#
# Первая: Ubuntu собирает libxcb-image.a и libxcb-util.a БЕЗ -fPIC, и в
# PIE-программу их не вложить вовсе.
#
# Вторая важнее и касается только курсора, но решает за всех. У libxcb-cursor
# в код вшит список каталогов с темами, и focal'ьный отстал: в нём нет
# ~/.local/share/icons, куда GNOME и KDE кладут пользовательские темы сегодня.
# Вшив его, мы получили бы программу, молча не видящую тему курсора. Остальные
# пять — чистые счётные помощники (ни одного зашитого пути, публичный API не
# менялся с 20.04), и берутся из upstream просто за компанию, одним правилом.
#
# Собирает их packaging/build-xcb-static.sh; там же объяснено, почему у
# курсора обязателен --with-cursorpath.
set(zsys_own_lib "$ENV{ZPREFIX}/lib")

# Пары перечислены АРГУМЕНТАМИ foreach, а не через переменную-список: CMake
# разворачивает ';' внутри элементов при `IN LISTS`, и пары рассыпаются в
# плоский список (наступил на это здесь же).
foreach(pair "XCB_CURSOR_LIBRARY;${zsys_own_lib}/libxcb-cursor.a"
             "XCB_ICCCM_LIBRARY;${zsys_own_lib}/libxcb-icccm.a"
             "XCB_KEYSYMS_LIBRARY;${zsys_own_lib}/libxcb-keysyms.a"
             "XCB_RENDERUTIL_LIBRARY;${zsys_own_lib}/libxcb-render-util.a"
             "XCB_IMAGE_LIBRARY;${zsys_own_lib}/libxcb-image.a"
             "XCB_UTIL_LIBRARY;${zsys_own_lib}/libxcb-util.a"
             "XCB_ATOM_LIBRARY;${zsys_own_lib}/libxcb-util.a"
             "XCB_AUX_LIBRARY;${zsys_own_lib}/libxcb-util.a")
    list(GET pair 0 zsys_var)
    list(GET pair 1 zsys_path)
    if(EXISTS "${zsys_path}")
        set(${zsys_var} "${zsys_path}" CACHE FILEPATH "вшито статически" FORCE)
    else()
        message(WARNING
            "нет ${zsys_path} — ${zsys_var} останется динамической.\n"
            "  Соберите: packaging/build-xcb-static.sh")
    endif()
endforeach()

# OpenMP — СТАТИЧЕСКОЙ libgomp. Иначе готовая программа требует libgomp.so.1
# (пакет libgomp1, priority=optional): на десктопе он обычно есть, но «обычно»
# — не то слово, которое хочется слышать про программу, раздаваемую одним
# файлом. Своё добро мы вшиваем, а OpenMP здесь именно своё: им распараллелены
# ресемплер и перевод цвета в zametti-core.
#
# Путь спрашиваем у компилятора, а не пишем: он зависит от версии gcc, и
# вписанный руками разъедется на первом же обновлении sysroot.
execute_process(COMMAND "$ENV{ZSYS}/bin/gcc" -print-file-name=libgomp.a
                OUTPUT_VARIABLE zsys_libgomp
                OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_QUIET)
if(zsys_libgomp AND EXISTS "${zsys_libgomp}")
    set(OpenMP_gomp_LIBRARY "${zsys_libgomp}" CACHE FILEPATH "статическая libgomp" FORCE)
endif()

# Статические библиотеки уедут внутрь PIE-программы.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
