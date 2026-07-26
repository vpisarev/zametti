# Проверка, что default-config.json в репозитории не разошёлся с кодом.
# Файл пользователь копирует к себе по частям, когда появляются новые параметры,
# поэтому устаревать ему нельзя.

execute_process(COMMAND ${APP} --dump-config OUTPUT_VARIABLE ACTUAL RESULT_VARIABLE RC)
if(NOT RC EQUAL 0)
    message(FATAL_ERROR "zametti --dump-config завершился с кодом ${RC}")
endif()

file(READ ${EXPECTED} STORED)
if(NOT ACTUAL STREQUAL STORED)
    message(FATAL_ERROR
            "default-config.json разошёлся с умолчаниями в коде.\n"
            "Обновить: ${APP} --dump-config > ${EXPECTED}")
endif()
