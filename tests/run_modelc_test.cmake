# Конвертер моделей как его запускает человек: корректный OBJ даёт `.lo`
# с сигнатурой формата и код 0, не-модель — код 1, неизвестная опция — 2.
foreach(variable MODELC INPUT_OBJ WORK_DIRECTORY)
    if(NOT DEFINED ${variable})
        message(FATAL_ERROR "${variable} is required")
    endif()
endforeach()

file(MAKE_DIRECTORY "${WORK_DIRECTORY}")
set(output "${WORK_DIRECTORY}/converted.lo")
file(REMOVE "${output}")
execute_process(COMMAND "${MODELC}" "${INPUT_OBJ}" "${output}"
    RESULT_VARIABLE result OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
if(NOT result EQUAL 0 OR NOT EXISTS "${output}")
    message(FATAL_ERROR "modelc failed on a valid OBJ (${result}): ${stdout}${stderr}")
endif()
file(READ "${output}" magic LIMIT 4 HEX)
if(NOT magic STREQUAL "4c4d4431")
    message(FATAL_ERROR "modelc output has no LMD1 signature: ${magic}")
endif()

set(not_a_model "${WORK_DIRECTORY}/not_a_model.obj")
file(WRITE "${not_a_model}" "hello, this is not a model\n")
execute_process(COMMAND "${MODELC}" "${not_a_model}" "${WORK_DIRECTORY}/rejected.lo"
    RESULT_VARIABLE result OUTPUT_QUIET ERROR_QUIET)
if(NOT result EQUAL 1 OR EXISTS "${WORK_DIRECTORY}/rejected.lo")
    message(FATAL_ERROR "modelc accepted a file that is not a model (${result})")
endif()

execute_process(COMMAND "${MODELC}" --no-such-option a b
    RESULT_VARIABLE result OUTPUT_QUIET ERROR_QUIET)
if(NOT result EQUAL 2)
    message(FATAL_ERROR "modelc did not report a usage error (${result})")
endif()
