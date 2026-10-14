# Записывает байткод шейдера в заголовок массивом слов uint32_t. Формат
# один для DXIL и SPIR-V: слова дают выравнивание, которого требует
# VkShaderModuleCreateInfo::pCode, а в файле нет ничего, кроме самих
# байтов, — ни дизассемблера, ни строк компилятора. Переносы строк всегда
# LF, поэтому заголовок с Windows и с Linux совпадает байт в байт.
foreach(variable IN ITEMS INPUT_FILE OUTPUT_FILE SYMBOL)
    if(NOT DEFINED ${variable} OR "${${variable}}" STREQUAL "")
        message(FATAL_ERROR "${variable} is required")
    endif()
endforeach()

file(READ "${INPUT_FILE}" hex HEX)
string(LENGTH "${hex}" digits)
math(EXPR remainder "${digits} % 8")
if(digits EQUAL 0 OR NOT remainder EQUAL 0)
    message(FATAL_ERROR "${INPUT_FILE}: bytecode size must be a non-zero multiple of 4")
endif()

# Little-endian: байты b0 b1 b2 b3 дают слово 0xb3b2b1b0.
string(REGEX REPLACE "(..)(..)(..)(..)" "0x\\4\\3\\2\\1," words "${hex}")
# Восемь слов в строке. Квантификаторов {n} в регулярных выражениях CMake
# нет, а запись слова всегда 11 символов: «0x» + 8 цифр + запятая.
string(REPEAT "." 88 line)
string(REGEX REPLACE "(${line})" "\\1\n    " words "${words}")
string(REGEX REPLACE "[ \n]+$" "" words "${words}")

file(CONFIGURE OUTPUT "${OUTPUT_FILE}"
    CONTENT "#pragma once\n\n#include <stdint.h>\n\nstatic const uint32_t ${SYMBOL}[] = {\n    ${words}\n};\n"
    @ONLY NEWLINE_STYLE LF)
