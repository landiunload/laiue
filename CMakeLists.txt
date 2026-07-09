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
# Статическая библиотека runtime: memset/memcpy/_fltused.
# Без /GL, чтобы MSVC не падал с C2268 на LTCG при определении этих функций.
add_library(laiue_runtime STATIC src/runtime/memory.c)
target_include_directories(laiue_runtime PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(laiue_runtime PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(laiue_runtime PRIVATE $<$<AND:$<C_COMPILER_ID:MSVC>,$<NOT:$<CONFIG:Debug>>>:/GL->)
target_compile_options(laiue_runtime PUBLIC /W4 /utf-8 /GS- /fp:fast $<$<CONFIG:Debug>:/Od> $<$<NOT:$<CONFIG:Debug>>:/O2 /Ot /Oi /Gw>)

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
    target_link_libraries(${module_name} PRIVATE laiue_common laiue_runtime)
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

# ========== DLL: world ==========
laiue_add_module(laiue_world
    src/world/world.c
    src/world/noise.c
    src/world/world.h
    src/api.h
)
target_compile_definitions(laiue_world PRIVATE LAIUE_BUILD_WORLD)
target_link_libraries(laiue_world PRIVATE kernel32)

# ========== Shader compilation ==========
set(FXC "$ENV{ProgramFiles\(x86\)}/Windows Kits/10/bin/10.0.20348.0/x64/fxc.exe")
if(NOT EXISTS "${FXC}")
    message(WARNING "fxc.exe not found at ${FXC}, shaders will not be compiled")
endif()

set(SHADER_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/shaders/chunk.hlsl")
set(GENERATED_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/render/generated")

add_custom_command(
    OUTPUT "${GENERATED_DIR}/chunk_vs.h" "${GENERATED_DIR}/chunk_ps.h"
    COMMAND "${FXC}" /nologo /T vs_5_0 /E VSMain /O3 /Qstrip_debug /Qstrip_reflect
        "/Fh${GENERATED_DIR}/chunk_vs.h" /Vn g_chunk_vs "${SHADER_SOURCE}"
    COMMAND "${FXC}" /nologo /T ps_5_0 /E PSMain /O3 /Qstrip_debug /Qstrip_reflect
        "/Fh${GENERATED_DIR}/chunk_ps.h" /Vn g_chunk_ps "${SHADER_SOURCE}"
    DEPENDS "${SHADER_SOURCE}"
    COMMENT "Compiling shaders..."
)

# Pre-compiled shader headers — no need to compile them as C
set(SHADER_HEADERS
    "${GENERATED_DIR}/chunk_vs.h"
    "${GENERATED_DIR}/chunk_ps.h"
)

# ========== DLL: render ==========
laiue_add_module(laiue_render
    src/render/renderer_d3d12.c
    src/render/renderer_d3d12_builder.c
    src/render/renderer.h
    src/api.h
    ${SHADER_HEADERS}
)
target_compile_definitions(laiue_render PRIVATE LAIUE_BUILD_RENDER)
target_link_libraries(laiue_render PRIVATE kernel32 d3d12.lib dxgi.lib dxguid.lib laiue_world)

# ========== DLL: core ==========
laiue_add_module(laiue_core
    src/core/application.c
    src/core/math.c
    src/game/camera.c
    src/api.h
)
target_compile_definitions(laiue_core PRIVATE LAIUE_BUILD_CORE)
target_link_libraries(laiue_core PRIVATE kernel32 laiue_window laiue_input laiue_world laiue_render)

# ========== EXE: launcher ==========
add_executable(laiue WIN32
    src/launcher/main.c
)
target_link_libraries(laiue PRIVATE laiue_common laiue_runtime kernel32)
# /STACK и /HEAP имеют смысл только у EXE — размеры берутся из его заголовка.
target_link_options(laiue PRIVATE
    /ENTRY:EntryPoint
    /STACK:262144,4096
    /HEAP:65536,4096
)
