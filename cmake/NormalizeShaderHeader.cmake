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
# fxc с /Vn и glslang с --vn дают массиву байткода внешнюю связность:
# `const BYTE g_<имя>[]` и `const uint32_t g_<имя>[]`. В двухбэкендной
# сборке обе реализации линкуются в один образ, и одинаковые внешние имена
# с разным типом нарушают ODR (MSVC: C4742/C4743, затем LNK1257). Байткод
# нужен ровно одной единице трансляции — своему бэкенду, поэтому
# объявление делается внутренним. Якорь — начало строки и только
# идентификатор g_[a-z_]+, чтобы не задеть пояснительный текст.
string(REGEX REPLACE "(^|\n)const (BYTE|uint32_t) (g_[a-z_]+\\[\\])"
    "\\1static const \\2 \\3" contents "${contents}")
file(WRITE "${INPUT_FILE}" "${contents}")
