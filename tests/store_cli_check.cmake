# СТОРОЖ ПОДКОМАНДЫ `zametti store` — командного вида хранилища.
#
# Юнит-тесты сюда не достают: они зовут ZStorage напрямую, а здесь проверяется
# то, чего у них нет, — диспетчер подкоманды, разбор ключей, коды возврата и
# сам факт, что командный режим стартует БЕЗ ДИСПЛЕЯ. До слияния программ
# (26.08.2026) командный режим не был покрыт ни одним тестом-процессом вовсе.
#
# Зовётся из tests/CMakeLists.txt: -DZAMETTI=<exe> -DWORK=<каталог>.

if(NOT ZAMETTI OR NOT WORK)
    message(FATAL_ERROR "нужны -DZAMETTI=<exe> и -DWORK=<dir>")
endif()

# Каждый прогон — с чистого листа: остатки прошлого дали бы «init» на непустом
# каталоге и зелень не по делу.
file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}")
set(store "${WORK}/store")

# Один прогон программы: ожидаемый код возврата, ожидаемая подстрока (пусто —
# не проверять) и сами аргументы. Вывод показывается только при провале —
# зелёный набор молчит.
function(zs expect_code expect_text)
    execute_process(COMMAND "${ZAMETTI}" store ${ARGN}
                    RESULT_VARIABLE code
                    OUTPUT_VARIABLE out
                    ERROR_VARIABLE err)
    set(bad "")
    if(NOT code EQUAL expect_code)
        set(bad "код ${code}, ждали ${expect_code}")
    elseif(expect_text AND NOT "${out}${err}" MATCHES "${expect_text}")
        set(bad "в выводе нет «${expect_text}»")
    endif()
    if(bad)
        message(SEND_ERROR "zametti store ${ARGN}: ${bad}\n--- stdout ---\n${out}--- stderr ---\n${err}")
    endif()
endfunction()

# --- справка и ошибки употребления -----------------------------------------
#
# Справка, которую попросили словами, — не беда: stdout и ноль. Она же в ответ
# на бессмыслицу — stderr и двойка. Обе стороны проверяются: первая редакция
# диспетчера роняла `zametti store` в оконный путь и открывала окно.
zs(2 "usage:")
zs(2 "usage:" "такой-команды-нет")
zs(0 "zametti store init" "--help")

# --- жизнь хранилища с нуля -------------------------------------------------
zs(0 "" init "${store}")
zs(0 "\\.md" new --root "${store}")
zs(0 "storeId:" root init --root "${store}")
zs(0 "storeId:" root show --root "${store}")
zs(0 "" verify --root "${store}")

# Прореживание на живом хранилище: ходит по журналам и обязано молча
# согласиться, что чистить нечего.
zs(0 "journals" thin --root "${store}" --dry-run)

# Чужой каталог хранилищем не притворяется.
zs(1 "not a store" verify --root "${WORK}")

# Считать провалы самим незачем: message(SEND_ERROR) в режиме -P уже уводит
# код возврата cmake в ненулевой, и ctest видит красное.
