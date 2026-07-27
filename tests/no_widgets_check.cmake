# Требование брифа этапа 2: слой модели документа обходится QtGui, а тестовые
# бинарники не тянут QtWidgets. Проверяется по фактическим зависимостям
# собранного бинарника, а не по include: виджет мог бы приехать транзитивно.

execute_process(COMMAND ldd "${BINARY}" OUTPUT_VARIABLE DEPS RESULT_VARIABLE RC
                ERROR_QUIET)
if(NOT RC EQUAL 0)
    message(STATUS "ldd недоступен, проверка пропущена")
    return()
endif()

if(DEPS MATCHES "libQt[0-9]*Widgets")
    message(FATAL_ERROR "в ${BINARY} приехал QtWidgets:\n${DEPS}")
endif()
message(STATUS "QtWidgets в ${BINARY} нет")
