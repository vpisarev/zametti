# Toolchain, которым собраны Qt и OpenSSL в $ZPREFIX. Подставляет его
# packaging/linux/qtbase-configure.sh; руками задавать не надо.
# Требует окружения из packaging/linux/zenv.sh (ZSYS, ZPREFIX).
#
# ДОЛГ, НАЗВАННЫЙ ВСЛУХ: этот файл и linux-zsys.cmake (которым собирается сама
# программа) почти одинаковы — разница ровно одна, там прибиты шесть
# XCB_*_LIBRARY к статическим архивам из $ZPREFIX. Два почти-одинаковых
# toolchain'а — заготовка расхождения; свести их в один можно, но это меняет
# то, чем собрана уже стоящая Qt, а значит требует её пересборки и проверки.
#
# CMAKE_SYSTEM_NAME НЕ ЗАДАЁМ НАРОЧНО. Архитектура та же, сборка остаётся
# «родной», и Qt не требует QT_HOST_PATH с отдельным хостовым Qt. Задать его —
# значит объявить кросс-сборку и завести себе вторую сборку Qt на пустом месте.

set(CMAKE_SYSROOT "$ENV{ZSYS}")
set(CMAKE_C_COMPILER "$ENV{ZSYS}/bin/gcc")
set(CMAKE_CXX_COMPILER "$ENV{ZSYS}/bin/g++")

# ГЛАВНОЕ. Без этого find_library() спокойно берёт /usr/lib/x86_64-linux-gnu
# ХОСТА (Ubuntu 26.04, glibc 2.43), сборка проходит, а переносимость утекает
# молча — ровно как утекала через абсолютные симлинки внутри sysroot.
set(CMAKE_FIND_ROOT_PATH "$ENV{ZSYS}" "$ENV{ZPREFIX}")
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
# А вот программы (perl, python, ninja, wayland-scanner) нужны ХОСТОВЫЕ:
# они исполняются здесь и сейчас, а не на целевой машине.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)

# Обязательны: libstdc++ у gcc-15 требует GLIBCXX_3.4.32, которого нет даже в
# Ubuntu 20.04. Без этого собранное не запустится на целевой системе.
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static-libstdc++ -static-libgcc")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-static-libstdc++ -static-libgcc")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "-static-libstdc++ -static-libgcc")

# Статические библиотеки уедут внутрь PIE-программы.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
