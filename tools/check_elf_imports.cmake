if(NOT DEFINED READELF_TOOL OR READELF_TOOL STREQUAL "" OR
   NOT DEFINED BINARY_FILE OR BINARY_FILE STREQUAL "")
    message(FATAL_ERROR "READELF_TOOL and BINARY_FILE are required")
endif()
if(NOT EXISTS "${BINARY_FILE}")
    message(FATAL_ERROR "ELF binary does not exist: ${BINARY_FILE}")
endif()

execute_process(
    COMMAND "${READELF_TOOL}" -d "${BINARY_FILE}"
    RESULT_VARIABLE readelf_result
    OUTPUT_VARIABLE dynamic_section
    ERROR_VARIABLE readelf_error)
if(NOT readelf_result EQUAL 0)
    message(FATAL_ERROR
        "Could not inspect ELF imports for ${BINARY_FILE}: ${readelf_error}")
endif()

# Модули и приложение получают соседние технологии только через таблицы
# сервисов. Загрузочный импорт другой библиотеки движка превратил бы
# удаление её артефакта в отказ запуска процесса, а не в отключение одной
# технологии.
string(REGEX MATCHALL "\\(NEEDED\\)[^\n]*\\[[^]\n]*\\]" needed_entries "${dynamic_section}")
foreach(entry IN LISTS needed_entries)
    string(REGEX REPLACE ".*\\[([^]]*)\\].*" "\\1" library "${entry}")
    if(library MATCHES "^liblaiue_")
        message(FATAL_ERROR
            "${BINARY_FILE} has a load-time import of engine module ${library}")
    endif()
endforeach()
