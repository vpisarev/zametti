# ЗАГЛУШКА, положена скриптом 3rdparty/libjxl/update.sh.
#
# Настоящий tools/CMakeLists.txt библиотеки заводит два десятка утилит
# командной строки и стендов; нам из всего каталога нужны три пары файлов
# (ssimulacra2, gauss_blur, no_memory_manager), и собираем мы их сами — см.
# 3rdparty/libjxl/CMakeLists.txt.
#
# Пустым этот файл быть обязан, а существовать — тоже обязан: в корневом
# CMakeLists.txt библиотеки стоит безусловный add_subdirectory(tools).
