# Механическая проверка правила владельца: getDocument() РАЗРЕШЁН РОВНО В ОДНОМ
# ВИДЕ.
#
#     view->setDocument(note.getDocument());
#
# Во всех остальных случаях он запрещён. Живой QTextDocument отдаётся наружу
# только затем, чтобы его показать; всё, что делается с заметкой, делается
# методами заметки. Пройти мимо них через этот указатель — значит завести вторую
# точку правки, о которой заметка не знает, и однажды разойтись с собственным
# содержимым.
#
# Правило записано в CLAUDE.md заглавными буквами, и владелец сказал прямо, что
# иначе ему придётся ловить меня руками каждый раз. Сторож переводит ловлю из
# обещания в сборку.
#
# Проверяем ИСХОДНИКИ, а не компоновку: правило про намерение. Объявление и
# определение самого метода, понятно, не в счёт — они и есть люк.

if(NOT SOURCE_DIRS)
    message(FATAL_ERROR "сторожу не задали SOURCE_DIRS")
endif()

set(OFFENDERS "")
set(ALLOWED 0)
set(CHECKED 0)

foreach(dir ${SOURCE_DIRS})
    if(NOT IS_DIRECTORY "${dir}")
        continue()
    endif()
    file(GLOB_RECURSE SOURCES "${dir}/*.h" "${dir}/*.cpp")
    foreach(src ${SOURCES})
        # Сам люк: там, где он объявлен и определён, звать его не пытаются.
        # И проверка самого люка — ей звать его положено, иначе проверять
        # нечего. Исключение названо ОДНИМ файлом нарочно: попадёт вызов в
        # любой другой набор — сторож скажет.
        get_filename_component(name "${src}" NAME)
        if(name STREQUAL "document.h" OR name STREQUAL "document.cpp"
           OR name STREQUAL "zdocument_test.cpp"
           # Стенд paste меряет ЧУЖУЮ вёрстку (QPlainTextDocumentLayout) на
           # нашем документе: QPlainTextEdit требует подменить вёрстку ДО
           # setDocument, и мимо люка этого не сделать. Это прибор, а не
           # продукт; исключение, как и у проверки люка, названо одним файлом.
           OR name STREQUAL "paste_bench.cpp")
            continue()
        endif()
        math(EXPR CHECKED "${CHECKED} + 1")
        file(STRINGS "${src}" LINES REGEX "getDocument[ \t]*\\(")
        foreach(line ${LINES})
            # Разрешено только внутри setDocument(...). Пробелы между именем и
            # скобкой допускаем: форматтер вправе их поставить. Пустые пары
            # скобок по пути — тоже: с сессии 3 законный вид зовётся через
            # заметку, view->setDocument(note.doc().getDocument()), и прежний
            # регэксп ([^)]*) спотыкался о скобки doc() — сторож краснел на
            # разрешённой форме.
            if(line MATCHES "setDocument[ \t]*\\((\\(\\)|[^()])*getDocument[ \t]*\\(")
                math(EXPR ALLOWED "${ALLOWED} + 1")
            else()
                string(STRIP "${line}" trimmed)
                list(APPEND OFFENDERS "${src}: ${trimmed}")
            endif()
        endforeach()
    endforeach()
endforeach()

if(OFFENDERS)
    string(REPLACE ";" "\n  " REPORT "${OFFENDERS}")
    message(FATAL_ERROR
        "getDocument() разрешён ТОЛЬКО как setDocument(zdoc.getDocument()).\n"
        "Здесь он зовётся иначе — значит с заметкой работают мимо её методов:\n  "
        "${REPORT}")
endif()
message(STATUS "getDocument() зовут только для показа: файлов ${CHECKED}, "
               "законных вызовов ${ALLOWED}")
