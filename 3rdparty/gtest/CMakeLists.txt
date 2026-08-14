# googletest 1.15.2, вендоринг подмножеством дерева. Взято из
# https://github.com/google/googletest, тег v1.15.2; чем обрезано — см.
# update.sh рядом.
#
# ЗАЧЕМ. До этого у наборов была своя обвязка на сотню строк (test_util.h:
# ZT_EQ, ZT_TRUE, счётчик провалов) и по отдельному исполняемому файлу на
# каждый набор — восемьдесят два штуки. Решение владельца: перевести всё на
# gtest и гонять ОДНИМ процессом.
#
# Смысл одного процесса не в удобстве, а в проверке: если набор портит память
# или течёт, это скажется на следующем — будет повод разобраться. Восемьдесят
# два отдельных процесса такое прятали по построению.
#
# Фильтр gtest даёт и обратное: --gtest_filter='Journal.*' гоняет один набор,
# как раньше гонялся один бинарник.
#
# ТОЛЬКО gtest, БЕЗ gmock. Заглушки и ожидания вызовов нам не нужны: наборы
# проекта проверяют настоящий код на настоящих данных — на корпусе заметок
# владельца, на его же полотнах Эрмитажа, на журналах с диска. Разговор
# объекта с выдуманным соседом здесь ничего не доказывает.
#
# ПОЧЕМУ СВОЙ CMake, А НЕ ЧУЖОЙ. Исходников у gtest два файла-амальгамы, а в
# корне чужого дерева живут option(BUILD_SHARED_LIBS), свои политики и
# установка — и всё это легло бы в наш кэш. Перечислить два файла дешевле,
# чем потом разбираться, что оттуда приехало. Тот же приём, что с md4c,
# blake3 и zstd.
set(GTEST_VENDORED_VERSION "1.15.2" PARENT_SCOPE)

add_library(gtest STATIC upstream/googletest/src/gtest-all.cc)
target_include_directories(gtest PRIVATE upstream/googletest)
# SYSTEM: заголовки чужие, и предупреждения по ним выписываться не должны —
# иначе -Werror в наших наборах падал бы на чужом коде.
target_include_directories(gtest SYSTEM PUBLIC upstream/googletest/include)
set_target_properties(gtest PROPERTIES POSITION_INDEPENDENT_CODE ON)

find_package(Threads QUIET)
if(Threads_FOUND)
    target_link_libraries(gtest PUBLIC Threads::Threads)
endif()

# Чужой код чужими правилами: zametti-code-rules ему не цепляется.
#
# Своего main gtest не даёт: он у нас общий на все наборы и живёт в
# tests/main.cpp — там же, где заводится QApplication и ставится платформа.
# Цель gtest_main из чужого дерева нам поэтому не нужна вовсе.
