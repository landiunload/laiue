if(NOT DEFINED NM_TOOL OR NM_TOOL STREQUAL "" OR
   NOT DEFINED LIBRARY_FILE OR LIBRARY_FILE STREQUAL "")
    message(FATAL_ERROR "NM_TOOL and LIBRARY_FILE are required")
endif()
if(NOT EXISTS "${LIBRARY_FILE}")
    message(FATAL_ERROR "ELF library does not exist: ${LIBRARY_FILE}")
endif()

execute_process(
    COMMAND "${NM_TOOL}" -D --defined-only "${LIBRARY_FILE}"
    RESULT_VARIABLE nm_result
    OUTPUT_VARIABLE dynamic_symbols
    ERROR_VARIABLE nm_error)
if(NOT nm_result EQUAL 0)
    message(FATAL_ERROR
        "Could not inspect ELF exports for ${LIBRARY_FILE}: ${nm_error}")
endif()

string(REPLACE "\r\n" "\n" dynamic_symbols "${dynamic_symbols}")
# Внутренние статические границы движка не должны попадать в публичный
# ABI: Platform* принадлежит platform_support, Scalar* — math_support.
foreach(internal_prefix Platform Scalar)
    string(REGEX MATCH "(^|\n)[^\n]*[ \t]${internal_prefix}[A-Za-z0-9_]*($|\n)"
        leaked_symbol "${dynamic_symbols}")
    if(NOT leaked_symbol STREQUAL "")
        string(STRIP "${leaked_symbol}" leaked_symbol)
        message(FATAL_ERROR
            "Internal ${internal_prefix} symbol leaked from ${LIBRARY_FILE}: "
            "${leaked_symbol}")
    endif()
endforeach()
