# ТАБ'У: УДАЛЕНИЕ, ПЕРЕИМЕНОВАНИЕ И ЗАПУСК ЧУЖИХ ПРОГРАММ — ТОЛЬКО ЧЕРЕЗ ZSystem.
#
# Правило владельца, вписанное первыми строками CLAUDE.md. Оно родилось из
# инцидента 30.08.2026: программа снесла каталог владельца, потому что
# спрашивать «а точно ли это наше» было не у кого — QFile::remove лежал в
# пятнадцати файлах, и каждый решал сам.
#
# Сторож — не украшение к обещанию, а его исполнение: правило, за которым не
# следит сборка, живёт до первой торопливой правки.
#
# ЧТО ЛОВИМ. Только разрушительное и только выход наружу: удаление,
# переименование, снос каталога, запуск чужой программы. Создание и запись
# (QSaveFile, mkpath, QFile::copy на новое имя) — свободны нарочно: худшее, что
# делает промах при создании, — лишний файл, а таб'у, запрещающее всё,
# перестаёт читаться как таб'у.
#
# Проверяются ИСХОДНИКИ, а не компоновка: правило про намерение. Комментарий —
# не вызов, и хвост от «//» отрезается прежде решения (на этих граблях молча
# краснел сторож getDocument, см. его шапку).

if(NOT SOURCE_DIRS)
    message(FATAL_ERROR "сторожу не задали SOURCE_DIRS")
endif()

# Имя функции → как его звать законно. Ровно эти имена и ищем.
set(FORBIDDEN
    # QFile::link создаёт, а не разрушает, и в таб'у не входит: список держится
    # узким нарочно (см. шапку).
    "QFile::remove"          "QFile::rename"          "QFile::moveToTrash"
    "QDir::rmdir"            "QDir::rmpath"           "QDir::remove"
    "removeRecursively"
    "QProcess"
    "std::remove"            "std::rename"
    "::unlink"               "::rmdir"                "::rename"
    # system — и голое, и через пространство имён: граница слова слева не
    # пускает двоеточие, и «std::system(» проходило мимо голого «system»
    # (нашлось 04.09.2026 на zstd_test.cpp — rm -rf через std::system).
    "system"                 "std::system"            "::system"
    "popen"
    "execl"                  "execlp"                 "execle"
    "execv"                  "execvp"                 "execvpe"
    "_wunlink"               "_wremove"               "_wrename")

# ОСТРОВА. Каждый назван ОДНИМ файлом нарочно: попадёт вызов в любой другой —
# сторож скажет.
#
#   zsystem.h / zsystem.cpp — сама дверь; ей эти вызовы и положены.
#
#   note_id.cpp — Qt-свободный примитив writeNewFile. Он берёт ::open с O_EXCL,
#       потому что «проверить, что имени нет, и создать» обязано быть ОДНИМ
#       действием ядра, а QFile/QSaveFile такого не умеют; его ::unlink убирает
#       ровно тот файл, который эта же функция создала строкой выше, и никакой
#       другой. Каталогов не касается вовсе.
#
#   file_ops_check.cmake сюда не входит: сторож — cmake, а не C++.
set(ISLANDS "zsystem.h" "zsystem.cpp" "note_id.cpp")

set(OFFENDERS "")
set(CHECKED 0)
set(ISLAND_FILES 0)

foreach(dir ${SOURCE_DIRS})
    if(NOT IS_DIRECTORY "${dir}")
        continue()
    endif()
    file(GLOB_RECURSE SOURCES "${dir}/*.h" "${dir}/*.cpp")
    foreach(src ${SOURCES})
        get_filename_component(name "${src}" NAME)
        list(FIND ISLANDS "${name}" island)
        if(NOT island EQUAL -1)
            math(EXPR ISLAND_FILES "${ISLAND_FILES} + 1")
            continue()
        endif()
        math(EXPR CHECKED "${CHECKED} + 1")
        # ЧИТАЕМ ФАЙЛ ЦЕЛИКОМ И РЕЖЕМ САМИ. file(STRINGS) для этого не годится:
        # он отдаёт СПИСОК, а список cmake разделён точкой с запятой — и всякая
        # `;` внутри строки исходника (а в C++ она в каждой второй) дробит её на
        # лишние элементы. Номера строк от этого уезжают, и сторож ругается,
        # показывая пальцем не туда: первая же редакция называла archive_test.cpp:367
        # там, где нарушение стояло на 363-й.
        # ENCODING UTF-8 обязателен: без него file(STRINGS) читает файл как
        # ASCII и рвёт строку по кириллице, отдавая хвост комментария уже без
        # его «//» — на этом молча краснел сторож getDocument (см. его шапку).
        #
        # НОМЕРА СТРОКИ ЗДЕСЬ НЕТ, И ЭТО РЕШЕНИЕ, А НЕ ЛЕНЬ. Считать её в cmake
        # надёжно не выходит: список разделён точкой с запятой (а `;` есть в
        # каждой второй строке C++), пустые элементы foreach молча пропускает, а
        # раскрытие переменной ещё раз проходит по обратным слэшам. Три попытки
        # дали три разных сдвига, и на store_test.cpp сторож называл 624-ю там,
        # где нарушение стоит на 627-й. Номер, который врёт, тычет пальцем в
        # невиновный код и стоит дороже, чем его отсутствие: сам текст строки
        # ниже находится поиском за одно нажатие.
        file(STRINGS "${src}" LINES ENCODING UTF-8)
        foreach(line ${LINES})
            # Комментарий — не вызов.
            string(REGEX REPLACE "//.*" "" code "${line}")
            if(code STREQUAL "")
                continue()
            endif()
            foreach(bad ${FORBIDDEN})
                # Экранируем спецзнаки; ищем ИМЯ, за которым идёт скобка, и с
                # границей слева — чтобы «system» в systemFont() и «remove» в
                # QHash::remove() не считались.
                string(REGEX REPLACE "([][+.*()^$?|\\])" "\\\\\\1" pattern "${bad}")
                if(NOT code MATCHES "(^|[^A-Za-z0-9_:])${pattern}[ \t]*\\(")
                    continue()
                endif()
                string(STRIP "${code}" trimmed)
                list(APPEND OFFENDERS "${src}: ${trimmed}")
            endforeach()
        endforeach()
    endforeach()
endforeach()

if(OFFENDERS)
    list(REMOVE_DUPLICATES OFFENDERS)
    string(REPLACE ";" "\n  " REPORT "${OFFENDERS}")
    message(FATAL_ERROR
        "\n"
        "=========================================================================\n"
        " ТАБ'У НАРУШЕНО: файлы удаляют, переименовывают или запускают чужие\n"
        " программы МИМО ZSystem.\n"
        "\n"
        " 30.08.2026 программа снесла каталог владельца именно потому, что\n"
        " разрушение было рассыпано по коду и спросить «это точно наше?» было\n"
        " не у кого. Дверь одна: zametti-core/store/zsystem.h.\n"
        "\n"
        "   удалить файл человека      files().remove(path, &error)\n"
        "   удалить служебный файл     files().removeForever(path, &error)\n"
        "   переименовать              files().rename(from, to, &error)\n"
        "   снести каталог             ZSystem::removeStorageTree(dir, &error)\n"
        "                              (и только если это хранилище zametti)\n"
        "   запустить утилиту          ZSystem::runTool(program, args)\n"
        "\n"
        " Найдено здесь:\n  "
        "${REPORT}\n"
        "=========================================================================\n")
endif()
message(STATUS "разрушительное — только через ZSystem: проверено файлов ${CHECKED}, "
               "островов ${ISLAND_FILES}")
