if(NOT DEFINED INPUT_FILE OR "${INPUT_FILE}" STREQUAL "")
    message(FATAL_ERROR "INPUT_FILE is required")
endif()

file(READ "${INPUT_FILE}" contents)
string(REPLACE "\r\n" "\n" contents "${contents}")
string(REPLACE "\r" "\n" contents "${contents}")
# glslang открывает заголовок строкой со своей версией и отступом перед
# #pragma once. Версия сделала бы checked-in fallback непроверяемым на
# другой машине, поэтому обе строки приводятся к каноническому виду.
string(REGEX REPLACE "^[ \t]*//[ \t]*[0-9]+\\.[0-9]+\\.[0-9]+[ \t]*\n" ""
    contents "${contents}")
string(REGEX REPLACE "(^|\n)[ \t]*#pragma once" "\\1#pragma once"
    contents "${contents}")
string(REPLACE "\t" "    " contents "${contents}")
string(REGEX REPLACE " +\n" "\n" contents "${contents}")
string(REGEX REPLACE "\n+$" "\n" contents "${contents}")
file(WRITE "${INPUT_FILE}" "${contents}")
