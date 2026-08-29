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
string(REGEX MATCH "(^|\n)[^\n]*[ \t]Platform[A-Za-z0-9_]*($|\n)"
    leaked_platform_symbol "${dynamic_symbols}")
if(NOT leaked_platform_symbol STREQUAL "")
    string(STRIP "${leaked_platform_symbol}" leaked_platform_symbol)
    message(FATAL_ERROR
        "Internal platform symbol leaked from ${LIBRARY_FILE}: "
        "${leaked_platform_symbol}")
endif()
