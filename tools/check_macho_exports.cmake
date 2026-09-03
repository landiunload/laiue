if(NOT DEFINED NM_TOOL OR NM_TOOL STREQUAL "" OR
   NOT DEFINED LIBRARY_FILE OR LIBRARY_FILE STREQUAL "")
    message(FATAL_ERROR "NM_TOOL and LIBRARY_FILE are required")
endif()
if(NOT EXISTS "${LIBRARY_FILE}")
    message(FATAL_ERROR "Mach-O library does not exist: ${LIBRARY_FILE}")
endif()

# Apple nm: -g keeps external symbols, -U omits undefined imports.  This is
# intentionally separate from the ELF checker because Darwin nm has no -D.
execute_process(
    COMMAND "${NM_TOOL}" -gU "${LIBRARY_FILE}"
    RESULT_VARIABLE nm_result
    OUTPUT_VARIABLE exported_symbols
    ERROR_VARIABLE nm_error)
if(NOT nm_result EQUAL 0)
    message(FATAL_ERROR
        "Could not inspect Mach-O exports for ${LIBRARY_FILE}: ${nm_error}")
endif()

string(REPLACE "\r\n" "\n" exported_symbols "${exported_symbols}")
# Внутренние статические границы движка не должны попадать в публичный
# ABI: Platform* принадлежит platform_support, Scalar* — math_support.
foreach(internal_prefix Platform Scalar)
    string(REGEX MATCH "(^|\n)[^\n]*[ \t]_?${internal_prefix}[A-Za-z0-9_]*($|\n)"
        leaked_symbol "${exported_symbols}")
    if(NOT leaked_symbol STREQUAL "")
        string(STRIP "${leaked_symbol}" leaked_symbol)
        message(FATAL_ERROR
            "Internal ${internal_prefix} symbol leaked from ${LIBRARY_FILE}: "
            "${leaked_symbol}")
    endif()
endforeach()
