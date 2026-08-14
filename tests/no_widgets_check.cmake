# Механическая проверка несущего правила: В ЯДРЕ НЕТ НИ ОДНОГО ВИДЖЕТА.
#
# Ядро сознательно зависит от QtGui — там живёт QTextDocument, и без него не
# обойтись (дизайн-документ, §15.1). Но от QtWidgets оно не зависит и зависеть
# не должно: всё, что знает про окна, события мыши и полосы прокрутки, живёт в
# zametti-ui, а Android получит своё.
#
# Прежде этот сторож звался view-without-widgets и работал через ldd по одному
# тестовому бинарнику. Он умер вместе с целью zametti-view, которая влилась в
# ядро; проверка исходников надёжнее — она ловит намерение, а не только то,
# что сегодня попало в компоновку.
#
# СПИСОК ВИДЖЕТОВ СПРАШИВАЕМ У САМОГО Qt, а не перечисляем руками. Первая
# редакция перечисляла, и первый же прогон поймал `#include <QList>` — потому
# что в списке стояло «List». Контейнер QtCore объявили виджетом; догадка
# вместо факта, ровно та ошибка, от которой предостерегает свод правил.

if(NOT WIDGETS_DIR OR NOT IS_DIRECTORY "${WIDGETS_DIR}")
    message(STATUS "ПРОПУЩЕНО: каталог заголовков QtWidgets не найден (${WIDGETS_DIR}) — "
                   "сверять не с чем")
    return()
endif()

# Имена заголовков QtWidgets, как их пишут в include: QWidget, QPushButton, …
file(GLOB WIDGET_HEADERS RELATIVE "${WIDGETS_DIR}" "${WIDGETS_DIR}/Q*")
list(LENGTH WIDGET_HEADERS WIDGET_COUNT)
if(WIDGET_COUNT EQUAL 0)
    message(STATUS "ПРОПУЩЕНО: в ${WIDGETS_DIR} нет заголовков вида Q* — сверять не с чем")
    return()
endif()

file(GLOB_RECURSE ZAMETTI_CORE_SOURCES "${CORE_DIR}/*.h" "${CORE_DIR}/*.cpp")
set(OFFENDERS "")
set(CHECKED 0)
foreach(src ${ZAMETTI_CORE_SOURCES})
    math(EXPR CHECKED "${CHECKED} + 1")
    # Форма <QtWidgets/...> ловится отдельно: она разрешается путями QtCore и
    # компилятору не мешает, а значит проскочила бы до самой компоновки.
    file(STRINGS "${src}" DIRECT REGEX "^[ \t]*#[ \t]*include[ \t]*[<\"]QtWidgets/")
    if(DIRECT)
        list(APPEND OFFENDERS "${src}: ${DIRECT}")
        continue()
    endif()
    file(STRINGS "${src}" INCLUDES REGEX "^[ \t]*#[ \t]*include[ \t]*[<\"]Q[A-Za-z]+[>\"]")
    foreach(line ${INCLUDES})
        string(REGEX MATCH "Q[A-Za-z]+" name "${line}")
        if(name IN_LIST WIDGET_HEADERS)
            list(APPEND OFFENDERS "${src}: ${line}")
        endif()
    endforeach()
endforeach()

if(OFFENDERS)
    message(FATAL_ERROR
        "в ядре найдены виджеты — им место в zametti-ui:\n${OFFENDERS}")
endif()
message(STATUS "ядро свободно от QtWidgets: файлов ${CHECKED}, известных виджетов ${WIDGET_COUNT}")
