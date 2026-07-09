cmake_minimum_required(VERSION 3.28)

project(laiue
    VERSION 0.2.0
    DESCRIPTION "Belaiu base - window and input"
    LANGUAGES C
)

if(NOT WIN32)
    message(FATAL_ERROR "laiue собирается только под Windows.")
endif()

if(NOT MSVC)
    message(FATAL_ERROR "Поддерживаются компиляторы с интерфейсом MSVC: cl.exe или clang-cl.exe.")
endif()

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# Флаги проекта рассчитаны на две конфигурации; RelWithDebInfo и
# MinSizeRel из мультиконфигурационных генераторов исключаются,
# а одноконфигурационным без явного выбора назначается Release.
get_property(LAIUE_IS_MULTI_CONFIG GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(LAIUE_IS_MULTI_CONFIG)
    set(CMAKE_CONFIGURATION_TYPES "Debug;Release" CACHE STRING "" FORCE)
elseif(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "" FORCE)
endif()

# Отладочная информация встраивается в объектные файлы (/Z7):
# нет коллизий vc*.pdb при параллельной сборке.
set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<$<CONFIG:Debug>:Embedded>")

# CMake по умолчанию добавляет /RTC1 в Debug — без CRT его заглушкам
# неоткуда взяться, поэтому вырезаем флаг целиком.
string(REPLACE "/RTC1" "" CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG}")

# В Release CMake добавляет /Ob2 — вырезаем, чтобы наш /Ob3
# не давал предупреждение D9025 о переопределении.
string(REPLACE "/Ob2" "" CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE}")

# CRT не линкуется — стандартный список библиотек не нужен,
# зависимости каждой цели заданы явно через target_link_libraries.
set(CMAKE_C_STANDARD_LIBRARIES "")

# === общие настройки всех модулей (единый источник истины) ===
add_library(laiue_common INTERFACE)

target_include_directories(laiue_common INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/src)

# Резервные memset/memcpy компилируются в каждый модуль:
# без CRT они обязаны быть в каждом DLL и в EXE.
target_sources(laiue_common INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/memory.c)

target_compile_definitions(laiue_common INTERFACE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    UNICODE
    _UNICODE
)

# cl.exe и clang-cl.exe принимают одинаковые базовые флаги;
# различия закрыты генераторными выражениями по C_COMPILER_ID.
# /Gw кладёт глобальные данные в отдельные COMDAT-секции —
# /OPT:REF выбрасывает неиспользуемые; на скорость не влияет.
target_compile_options(laiue_common INTERFACE
    /W4 /utf-8 /GS- /fp:fast
    $<$<CONFIG:Debug>:/WX /Od>
    $<$<NOT:$<CONFIG:Debug>>:/O2 /Ot /Oi /Gw>
    $<$<AND:$<C_COMPILER_ID:MSVC>,$<NOT:$<CONFIG:Debug>>>:/GL /Ob3>
    $<$<AND:$<C_COMPILER_ID:Clang>,$<NOT:$<CONFIG:Debug>>>:-flto=thin>
    $<$<C_COMPILER_ID:Clang>:-Wno-unused-command-line-argument>
)

# /EMITTOOLVERSIONINFO и /LTCG понимает только линкер MSVC;
# lld-link при -flto=thin обрабатывает bitcode сам, без флагов линкеру.
target_link_options(laiue_common INTERFACE
    /NODEFAULTLIB /SUBSYSTEM:WINDOWS
    /DYNAMICBASE:NO /HIGHENTROPYVA:NO
    /MANIFEST:NO
    /MERGE:.rdata=.text /MERGE:.pdata=.text
    $<$<C_COMPILER_ID:MSVC>:/EMITTOOLVERSIONINFO:NO>
    $<$<CONFIG:Debug>:/DEBUG /INCREMENTAL:NO>
    $<$<NOT:$<CONFIG:Debug>>:/OPT:REF /OPT:ICF>
    $<$<AND:$<C_COMPILER_ID:MSVC>,$<NOT:$<CONFIG:Debug>>>:/LTCG>
)

# Объявляет DLL проекта: общие настройки + /NOENTRY.
# CRT нет, инициализация не нужна — точка входа DllMain не требуется вовсе.
function(laiue_add_module module_name)
    add_library(${module_name} SHARED ${ARGN})
    target_link_libraries(${module_name} PRIVATE laiue_common)
    target_link_options(${module_name} PRIVATE /NOENTRY)
endfunction()

# ========== DLL: window ==========
laiue_add_module(laiue_window
    src/platform/window.c
    src/platform/window.h
    src/api.h
)
target_compile_definitions(laiue_window PRIVATE LAIUE_BUILD_WINDOW)
target_link_libraries(laiue_window PRIVATE kernel32 user32)

# ========== DLL: input ==========
laiue_add_module(laiue_input
    src/input/input_windows.c
    src/input/input.h
    src/api.h
)
target_compile_definitions(laiue_input PRIVATE LAIUE_BUILD_INPUT)
target_link_libraries(laiue_input PRIVATE kernel32 user32)

# ========== DLL: core ==========
laiue_add_module(laiue_core
    src/core/application.c
    src/api.h
)
target_compile_definitions(laiue_core PRIVATE LAIUE_BUILD_CORE)
target_link_libraries(laiue_core PRIVATE laiue_window laiue_input)

# ========== EXE: launcher ==========
add_executable(laiue WIN32
    src/launcher/main.c
)
target_link_libraries(laiue PRIVATE laiue_common kernel32)
# /STACK и /HEAP имеют смысл только у EXE — размеры берутся из его заголовка.
target_link_options(laiue PRIVATE
    /ENTRY:EntryPoint
    /STACK:262144,4096
    /HEAP:65536,4096
)
