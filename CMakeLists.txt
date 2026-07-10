cmake_minimum_required(VERSION 3.28)

project(laiue
    VERSION 0.3.0
    DESCRIPTION "Belaiu base - window, input, world, render"
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
# неоткуда взяться; /Ob2 в Release конфликтует с нашим /Ob3 (D9025).
string(REPLACE "/RTC1" "" CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG}")
string(REPLACE "/Ob2" "" CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE}")

# CRT не линкуется — стандартный список библиотек не нужен,
# зависимости каждой цели заданы явно.
set(CMAKE_C_STANDARD_LIBRARIES "")

# ============================================================
# Общие настройки (единый источник истины)
# ============================================================

# Настройки компиляции без /GL — их использует и laiue_runtime,
# которому /GL противопоказан (#pragma function не работает под LTCG).
add_library(laiue_compile_settings INTERFACE)
target_include_directories(laiue_compile_settings INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(laiue_compile_settings INTERFACE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    UNICODE
    _UNICODE
)
# cl.exe и clang-cl.exe принимают одинаковые базовые флаги;
# различия закрыты генераторными выражениями по C_COMPILER_ID.
# /Gw кладёт глобальные данные в COMDAT-секции — /OPT:REF выбрасывает
# неиспользуемые; на скорость не влияет.
target_compile_options(laiue_compile_settings INTERFACE
    /W4 /utf-8 /GS- /fp:fast
    $<$<CONFIG:Debug>:/WX /Od>
    $<$<NOT:$<CONFIG:Debug>>:/O2 /Ot /Oi /Gw>
    $<$<AND:$<C_COMPILER_ID:MSVC>,$<NOT:$<CONFIG:Debug>>>:/Ob3>
    $<$<AND:$<C_COMPILER_ID:Clang>,$<NOT:$<CONFIG:Debug>>>:-flto=thin>
    $<$<C_COMPILER_ID:Clang>:-Wno-unused-command-line-argument>
)

# Полные настройки модулей: компиляция + /GL + линковка.
# /EMITTOOLVERSIONINFO и /LTCG понимает только линкер MSVC;
# lld-link при -flto=thin обрабатывает bitcode сам, без флагов линкеру.
add_library(laiue_common INTERFACE)
target_link_libraries(laiue_common INTERFACE laiue_compile_settings)
target_compile_options(laiue_common INTERFACE
    $<$<AND:$<C_COMPILER_ID:MSVC>,$<NOT:$<CONFIG:Debug>>>:/GL>
)
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

# Резервные memset/memcpy и _fltused для сборки без CRT.
# Именно OBJECT-библиотека: ссылки на эти символы возникают на этапе
# кодогенерации LTO — уже после разрешения символов из архивов,
# поэтому STATIC-библиотеку линкер бы не вытянул. Объектный файл
# попадает в каждый модуль и EXE безусловно.
# Флаги заданы отдельно и намеренно БЕЗ LTO (/GL, -flto): под MSVC
# с /GL не работает #pragma function, а под thinLTO интернализация
# выбрасывает _fltused до того, как бэкенд сгенерирует ссылки на него.
add_library(laiue_runtime OBJECT src/runtime/memory.c)
target_compile_options(laiue_runtime PRIVATE
    /W4 /utf-8 /GS- /fp:fast
    $<$<CONFIG:Debug>:/Od>
    $<$<NOT:$<CONFIG:Debug>>:/O2 /Oi>
)

# ============================================================
# Объявление модуля одной командой:
#   laiue_add_module(<имя> SOURCES <файлы...> [LINK <библиотеки...>])
# Создаёт DLL laiue_<имя> с макросом экспорта LAIUE_BUILD_<ИМЯ>,
# общими настройками, runtime-библиотекой и /NOENTRY
# (CRT нет, инициализация не нужна — DllMain не требуется вовсе).
# ============================================================
function(laiue_add_module module_name)
    cmake_parse_arguments(MODULE "" "" "SOURCES;LINK" ${ARGN})

    set(target_name laiue_${module_name})
    string(TOUPPER ${module_name} module_name_upper)

    add_library(${target_name} SHARED ${MODULE_SOURCES})
    target_compile_definitions(${target_name} PRIVATE LAIUE_BUILD_${module_name_upper})
    target_link_libraries(${target_name} PRIVATE laiue_common laiue_runtime kernel32 ${MODULE_LINK})
    target_link_options(${target_name} PRIVATE /NOENTRY)
endfunction()

# ============================================================
# Компиляция шейдеров: fxc ищется в PATH, затем берётся самая
# новая версия из установленных Windows SDK. Заголовки генерируются
# в дерево исходников и закоммичены — сборка работает и без fxc.
# ============================================================
find_program(LAIUE_SHADER_COMPILER fxc)
if(NOT LAIUE_SHADER_COMPILER)
    file(GLOB LAIUE_SHADER_COMPILER_CANDIDATES
        "$ENV{ProgramFiles\(x86\)}/Windows Kits/10/bin/10.*/x64/fxc.exe")
    if(LAIUE_SHADER_COMPILER_CANDIDATES)
        list(SORT LAIUE_SHADER_COMPILER_CANDIDATES COMPARE NATURAL)
        list(GET LAIUE_SHADER_COMPILER_CANDIDATES -1 LAIUE_SHADER_COMPILER)
    endif()
endif()

set(LAIUE_GENERATED_SHADER_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/src/render/generated")

# laiue_compile_shader(<имя>) — компилирует shaders/<имя>.hlsl
# (вершинный VSMain и пиксельный PSMain) в заголовки
# <имя>_vs.h / <имя>_ps.h с массивами g_<имя>_vs / g_<имя>_ps.
function(laiue_compile_shader shader_name)
    if(NOT LAIUE_SHADER_COMPILER)
        message(STATUS "fxc.exe не найден — используются закоммиченные заголовки шейдера «${shader_name}»")
        return()
    endif()

    set(shader_source "${CMAKE_CURRENT_SOURCE_DIR}/shaders/${shader_name}.hlsl")
    add_custom_command(
        OUTPUT
            "${LAIUE_GENERATED_SHADER_DIRECTORY}/${shader_name}_vs.h"
            "${LAIUE_GENERATED_SHADER_DIRECTORY}/${shader_name}_ps.h"
        COMMAND "${LAIUE_SHADER_COMPILER}" /nologo /T vs_5_0 /E VSMain /O3 /Qstrip_debug /Qstrip_reflect
            "/Fh${LAIUE_GENERATED_SHADER_DIRECTORY}/${shader_name}_vs.h" /Vn g_${shader_name}_vs "${shader_source}"
        COMMAND "${LAIUE_SHADER_COMPILER}" /nologo /T ps_5_0 /E PSMain /O3 /Qstrip_debug /Qstrip_reflect
            "/Fh${LAIUE_GENERATED_SHADER_DIRECTORY}/${shader_name}_ps.h" /Vn g_${shader_name}_ps "${shader_source}"
        DEPENDS "${shader_source}"
        COMMENT "Компиляция шейдера ${shader_name}"
    )
endfunction()

laiue_compile_shader(chunk)

# ============================================================
# Модули
# ============================================================

laiue_add_module(window
    SOURCES
        src/platform/window.c
        src/platform/window.h
        src/platform/time.c
        src/platform/time.h
        src/api.h
    LINK user32
)

laiue_add_module(input
    SOURCES src/input/input_windows.c src/input/input.h src/api.h
    LINK user32
)

laiue_add_module(world
    SOURCES src/world/world.c src/world/noise.c src/world/world.h src/world/noise.h src/api.h
)

laiue_add_module(mesher
    SOURCES
        src/mesh/chunk_mesher.c
        src/mesh/chunk_mesher.h
        src/render/chunk_geometry.h
        src/api.h
    LINK laiue_world
)

laiue_add_module(render
    SOURCES
        src/render/renderer_d3d12.c
        src/render/renderer.h
        src/render/chunk_geometry.h
        src/api.h
        "${LAIUE_GENERATED_SHADER_DIRECTORY}/chunk_vs.h"
        "${LAIUE_GENERATED_SHADER_DIRECTORY}/chunk_ps.h"
    LINK d3d12 dxgi dxguid
)

laiue_add_module(core
    SOURCES
        src/core/application.c
        src/core/chunk_streaming.c
        src/core/chunk_streaming.h
        src/core/math.c
        src/core/math.h
        src/game/camera.c
        src/game/camera.h
        src/api.h
    LINK laiue_window laiue_input laiue_world laiue_mesher laiue_render
)

# ============================================================
# EXE: лаунчер
# ============================================================
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
