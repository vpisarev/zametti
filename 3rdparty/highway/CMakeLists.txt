# highway 1.3.0, вендоринг. Взято из https://github.com/google/highway,
# коммит 271a9a0ed9de1232d9117f1572c3fe28f8542ec1 (20 января 2026) — на нём
# закреплён jpegli; libjxl 0.12.0 закрепил более старый (457c891, май 2024),
# и на новой собирается без единой ошибки (проверено).
#
# ПОЧЕМУ ОТДЕЛЬНЫМ КАТАЛОГОМ, А НЕ ВНУТРИ КАЖДОГО. Highway нужна и libjxl, и
# jpegli, и каждый несёт её своим подмодулем. Две копии в одной программе — это
# две статические библиотеки с одинаковыми именами символов: в лучшем случае
# линкер молча возьмёт первую, в худшем откажется собирать. Разводим один раз
# здесь и отдаём обоим (требование владельца).
#
# Highway — это SIMD: один и тот же исходник компилируется под несколько
# наборов команд, а нужный выбирается на исполнении. На этой машине выбирается
# AVX2 (проверено пробником); AVX512 процессор не умеет, и libjxl его и так
# отключает.
#
# Обновление: заменить upstream/ содержимым нужного коммита highway, выбросив
# .git, g3doc и docs; прогнать обе сборки — libjxl и jpegli опираются на неё
# по-разному, и сломаться может любая.

set(HWY_VENDORED_VERSION "1.3.0" PARENT_SCOPE)

# Чужие настройки — через кэш: у highway это option(), и обычная переменная
# сработала бы не всегда.
set(HWY_ENABLE_CONTRIB   OFF CACHE INTERNAL "выставлено zametti")
set(HWY_ENABLE_EXAMPLES  OFF CACHE INTERNAL "выставлено zametti")
set(HWY_ENABLE_TESTS     OFF CACHE INTERNAL "выставлено zametti")
set(HWY_ENABLE_INSTALL   OFF CACHE INTERNAL "выставлено zametti")
set(HWY_FORCE_STATIC_LIBS ON CACHE INTERNAL "выставлено zametti")
set(HWY_SYSTEM_GTEST     ON  CACHE INTERNAL "выставлено zametti")

add_subdirectory(upstream)

# libjxl обращается к цели как $<IF:TARGET_EXISTS:hwy::hwy, hwy::hwy, hwy>,
# jpegli — по-своему. Псевдоним закрывает оба случая разом.
if(NOT TARGET hwy::hwy)
    add_library(hwy::hwy ALIAS hwy)
endif()

# --- чтобы чужие сборки нашли ЭТУ highway, а не искали свою ----------------
#
# И libjxl, и jpegli устроены одинаково: есть свой third_party/highway — берут
# его; нет — зовут find_package(HWY 1.0.7). Мы их подкаталоги с highway
# выбросили, значит пойдут вторым путём. Подкладываем им пакет, который просто
# говорит «нашлось» и указывает на уже собранную цель.
#
# Так чужой CMake остаётся НЕТРОНУТЫМ: ни одной правки в их файлах, только
# переменные снаружи. Патченный чужой CMake — худшее, что можно оставить
# следующему обновлению.
# И у libjxl, и у jpegli в дереве лежит СВОЙ cmake/FindHWY.cmake, а модульный
# поиск идёт раньше пакетного — то есть подсунуть HWYConfig.cmake бесполезно
# (проверено: сборка падала на «Highway library (hwy) not found»). Зато их
# FindHWY требует ровно двух кэшируемых переменных и, найдя их, НИЧЕГО не
# создаёт, если цель hwy уже есть:
#
#     if (HWY_LIBRARY AND NOT TARGET hwy) ... endif()
#
# Значит достаточно заполнить кэш заранее. Версию их модуль вычитает из
# hwy/base.h нашего же дерева, и 1.3.0 их порог 1.0.7 перекрывает.
set(HWY_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/upstream"
    CACHE PATH "выставлено zametti: highway живёт в дереве, искать нечего" FORCE)
set(HWY_LIBRARY "$<TARGET_FILE:hwy>"
    CACHE FILEPATH "выставлено zametti" FORCE)
# Переменная нужна их модулю только чтобы «нашлось»; линкуются и libjxl, и
# jpegli всё равно через цель hwy — проверено по их CMake.

# Чужой код чужими правилами: zametti-warnings сюда не цепляется.
