# Механическая проверка правила владельца: ОКНА СООБЩЕНИЙ СТРОЯТСЯ В ОДНОМ
# МЕСТЕ — ZApp::messageBox / warn / inform / ask (app/zapp.cpp).
#
# Почему сторож, а не обещание: 04.09.2026 владелец увидел два вопроса
# программы в двух разных обликах — «Delete permanently?» шёл статическим
# QMessageBox::question (на маке — родной NSAlert со значком приложения и
# своими кнопками), вопрос истории — окном, собранным руками. Одинаковыми они
# останутся, только если собирать их негде, кроме одной двери.
#
# Ловим ПОСТРОЕНИЕ окна: статические question/warning/information/critical/
# about, `new QMessageBox`, QMessageBox на стеке. Спрашивать у готового окна
# (findChildren<QMessageBox*>, button(QMessageBox::Yes)) наборам можно.

if(NOT SOURCE_DIRS)
    message(FATAL_ERROR "сторожу не задали SOURCE_DIRS")
endif()

set(OFFENDERS "")
set(CHECKED 0)

foreach(dir ${SOURCE_DIRS})
    if(NOT IS_DIRECTORY "${dir}")
        continue()
    endif()
    file(GLOB_RECURSE SOURCES "${dir}/*.h" "${dir}/*.cpp")
    foreach(src ${SOURCES})
        get_filename_component(name "${src}" NAME)
        # Сама дверь.
        if(name STREQUAL "zapp.cpp")
            continue()
        endif()
        math(EXPR CHECKED "${CHECKED} + 1")
        file(STRINGS "${src}" LINES ENCODING UTF-8)
        set(n 0)
        foreach(line IN LISTS LINES)
            math(EXPR n "${n} + 1")
            if(line MATCHES "QMessageBox::(question|warning|information|critical|about)[ ]*\\("
               OR line MATCHES "new[ ]+QMessageBox"
               OR line MATCHES "(^|[^A-Za-z_:])QMessageBox[ ]+[A-Za-z_]+[ ]*[(;{]")
                list(APPEND OFFENDERS "${src}:${n}: ${line}")
            endif()
        endforeach()
    endforeach()
endforeach()

if(OFFENDERS)
    message(STATUS "окно сообщений построено мимо ZApp::messageBox:")
    foreach(o ${OFFENDERS})
        message(STATUS "  ${o}")
    endforeach()
    message(FATAL_ERROR "окна сообщений — только через ZApp::messageBox / warn / inform / ask (CLAUDE.md)")
endif()
message(STATUS "окна сообщений: проверено файлов ${CHECKED}, все через ZApp")
