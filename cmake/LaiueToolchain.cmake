include_guard(GLOBAL)

option(LAIUE_WARNINGS_AS_ERRORS "Считать предупреждения ошибками" ON)
option(LAIUE_ENABLE_LTO "Включить LTO в Release" ON)
option(LAIUE_ENABLE_SANITIZERS
    "Включить AddressSanitizer и UndefinedBehaviorSanitizer" OFF)

if(WIN32)
    if(NOT MSVC)
        message(FATAL_ERROR
            "Windows-сборка поддерживает cl.exe и clang-cl.exe")
    endif()
    # CMake добавляет старый набор Win32 libraries к каждой C-цели
    # неявно. Убираем его, чтобы зависимости объявлялись у конкретных
    # targets (platform/window/audio/render), а headless server не тянул UI.
    set(CMAKE_C_STANDARD_LIBRARIES "")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    if(NOT CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        message(FATAL_ERROR
            "Linux-сервер поддерживает GCC и Clang")
    endif()
else()
    message(FATAL_ERROR
        "Поддерживаются Windows и Linux; текущая система: ${CMAKE_SYSTEM_NAME}")
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "laiue поддерживает только 64-битные сборки")
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Linux"
   AND NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
    message(FATAL_ERROR
        "Linux-сервер пока поддерживает только x86_64; текущая архитектура: "
        "${CMAKE_SYSTEM_PROCESSOR}")
endif()

get_property(LAIUE_IS_MULTI_CONFIG GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(LAIUE_IS_MULTI_CONFIG)
    set(CMAKE_CONFIGURATION_TYPES "Debug;Release" CACHE STRING "" FORCE)
elseif(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "" FORCE)
endif()

# Общие правила компиляции. Все свойства намеренно target-scoped: выбор
# сервера не меняет глобальные флаги CMake и не загрязняет потребителей SDK.
add_library(laiue_build_options INTERFACE)
add_library(laiue::build_options ALIAS laiue_build_options)
target_include_directories(laiue_build_options
    INTERFACE "${PROJECT_SOURCE_DIR}/src")
target_compile_definitions(laiue_build_options INTERFACE
    "LAIUE_VERSION_TEXT=L\"${PROJECT_VERSION}\""
    LAIUE_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    LAIUE_VERSION_MINOR=${PROJECT_VERSION_MINOR}
)

if(WIN32)
    target_compile_definitions(laiue_build_options INTERFACE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        UNICODE
        _UNICODE
    )
    target_compile_options(laiue_build_options INTERFACE
        /W4 /utf-8 /GS- /fp:fast
        $<$<BOOL:${LAIUE_WARNINGS_AS_ERRORS}>:/WX>
        $<$<CONFIG:Debug>:/Od /RTC- /Z7>
        $<$<NOT:$<CONFIG:Debug>>:/O2 /Ot /Oi /Gw>
        $<$<AND:$<C_COMPILER_ID:MSVC>,$<NOT:$<CONFIG:Debug>>>:/Ob3>
        $<$<C_COMPILER_ID:Clang>:-Wno-unused-command-line-argument>
    )
else()
    target_compile_options(laiue_build_options INTERFACE
        -Wall -Wextra -Wpedantic
        $<$<BOOL:${LAIUE_WARNINGS_AS_ERRORS}>:-Werror>
        $<$<CONFIG:Debug>:-O0;-g3>
        $<$<NOT:$<CONFIG:Debug>>:-O2>
    )

    set(LAIUE_LINUX_LIBC "gnu" CACHE STRING
        "Linux libc ABI для server/mod artifacts: gnu или musl")
    set_property(CACHE LAIUE_LINUX_LIBC PROPERTY STRINGS gnu musl)
    if(LAIUE_LINUX_LIBC STREQUAL "gnu")
        target_compile_definitions(laiue_build_options INTERFACE
            LAIUE_LINUX_LIBC_GNU=1)
    elseif(LAIUE_LINUX_LIBC STREQUAL "musl")
        target_compile_definitions(laiue_build_options INTERFACE
            LAIUE_LINUX_LIBC_MUSL=1)
    else()
        message(FATAL_ERROR
            "LAIUE_LINUX_LIBC должен быть gnu или musl, получено: "
            "${LAIUE_LINUX_LIBC}")
    endif()

    if(LAIUE_ENABLE_SANITIZERS)
        target_compile_options(laiue_build_options INTERFACE
            -fno-omit-frame-pointer -fsanitize=address,undefined)
        target_link_options(laiue_build_options INTERFACE
            -fsanitize=address,undefined)
    endif()
endif()

# --- Точная плавающая арифметика для отдельных файлов ----------------------
#
# Недоверенный ввод не должен компилироваться с finite-math-only, а
# детерминированная симуляция дополнительно запрещает FMA-контракцию.
if(WIN32)
    set(LAIUE_PRECISE_FP_OPTIONS
        "/fp:precise"
        "$<$<C_COMPILER_ID:Clang>:-Wno-overriding-complex-range>")
    set(LAIUE_DETERMINISTIC_FP_OPTIONS
        ${LAIUE_PRECISE_FP_OPTIONS}
        "$<$<C_COMPILER_ID:Clang>:/clang:-ffp-contract=off>"
        "$<$<C_COMPILER_ID:Clang>:-Wno-overriding-option>")
else()
    set(LAIUE_PRECISE_FP_OPTIONS "-fno-fast-math")
    set(LAIUE_DETERMINISTIC_FP_OPTIONS
        "-fno-fast-math"
        "-ffp-contract=off")
endif()

# Windows no-CRT является отдельным opt-in контрактом. Linux-цели никогда не
# наследуют /NODEFAULTLIB или собственные memcpy/memset.
add_library(laiue_windows_no_crt INTERFACE)
add_library(laiue::windows_no_crt ALIAS laiue_windows_no_crt)
if(WIN32)
    add_library(laiue_runtime OBJECT
        "${PROJECT_SOURCE_DIR}/src/runtime/memory.c")
    target_compile_options(laiue_runtime PRIVATE
        /W4 /utf-8 /GS- /fp:fast
        $<$<BOOL:${LAIUE_WARNINGS_AS_ERRORS}>:/WX>
        $<$<CONFIG:Debug>:/Od /RTC- /Z7>
        $<$<NOT:$<CONFIG:Debug>>:/O2 /Oi>)
    target_sources(laiue_windows_no_crt INTERFACE
        "$<TARGET_OBJECTS:laiue_runtime>")
    target_link_options(laiue_windows_no_crt INTERFACE
        /NODEFAULTLIB
        /DYNAMICBASE /HIGHENTROPYVA /NXCOMPAT
        /MANIFEST:NO
        /MERGE:.rdata=.text /MERGE:.pdata=.text
        $<$<C_COMPILER_ID:MSVC>:/EMITTOOLVERSIONINFO:NO>
        $<$<CONFIG:Debug>:/DEBUG /INCREMENTAL:NO>
        $<$<NOT:$<CONFIG:Debug>>:/OPT:REF /OPT:ICF>
    )
else()
    # Совместимое имя избавляет старые локальные CMake-потребители от
    # платформенных if(); объектного no-CRT runtime на Linux нет.
    add_library(laiue_runtime INTERFACE)
endif()

if(LAIUE_ENABLE_LTO)
    if(WIN32)
        target_compile_options(laiue_build_options INTERFACE
            $<$<AND:$<C_COMPILER_ID:MSVC>,$<NOT:$<CONFIG:Debug>>>:/GL>
            $<$<AND:$<C_COMPILER_ID:Clang>,$<NOT:$<CONFIG:Debug>>>:-flto=thin>)
        target_link_options(laiue_build_options INTERFACE
            $<$<AND:$<C_COMPILER_ID:MSVC>,$<NOT:$<CONFIG:Debug>>>:/LTCG>)
    else()
        target_compile_options(laiue_build_options INTERFACE
            $<$<NOT:$<CONFIG:Debug>>:-flto>)
        target_link_options(laiue_build_options INTERFACE
            $<$<NOT:$<CONFIG:Debug>>:-flto>)
    endif()
endif()

# Обратная совместимость для существующих модулей и внешних локальных
# скриптов. Новые цели должны предпочитать namespaced build-options.
add_library(laiue_common INTERFACE)
target_link_libraries(laiue_common INTERFACE laiue::build_options)
if(WIN32)
    target_link_libraries(laiue_common INTERFACE laiue::windows_no_crt)
endif()
