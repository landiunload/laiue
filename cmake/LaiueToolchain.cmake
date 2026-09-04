include_guard(GLOBAL)

option(LAIUE_WARNINGS_AS_ERRORS "Считать предупреждения ошибками" ON)
option(LAIUE_ENABLE_LTO "Включить LTO в Release" ON)
option(LAIUE_AGGRESSIVE_INLINING
    "Использовать MSVC /Ob3 в Release speed profile" ON)
set(LAIUE_CLANG_LTO_MODE "full" CACHE STRING
    "Clang Release LTO mode: thin or full")
set_property(CACHE LAIUE_CLANG_LTO_MODE PROPERTY STRINGS thin full)
if(NOT LAIUE_CLANG_LTO_MODE MATCHES "^(thin|full)$")
    message(FATAL_ERROR
        "LAIUE_CLANG_LTO_MODE должен быть thin или full, получено: "
        "${LAIUE_CLANG_LTO_MODE}")
endif()
set(LAIUE_X86_64_LEVEL "avx2" CACHE STRING
    "Release ISA profile: sse2, MSVC AVX2/GNU x86-64-v3, or avx512")
set_property(CACHE LAIUE_X86_64_LEVEL PROPERTY STRINGS sse2 avx2 avx512)
if(NOT LAIUE_X86_64_LEVEL MATCHES "^(sse2|avx2|avx512)$")
    message(FATAL_ERROR
        "LAIUE_X86_64_LEVEL должен быть sse2, avx2 или avx512, получено: "
        "${LAIUE_X86_64_LEVEL}")
endif()
set(LAIUE_X86_64_TUNE "generic" CACHE STRING
    "Release CPU scheduling tune: generic or amd_zen4")
set_property(CACHE LAIUE_X86_64_TUNE PROPERTY STRINGS generic amd_zen4)
if(NOT LAIUE_X86_64_TUNE MATCHES "^(generic|amd_zen4)$")
    message(FATAL_ERROR
        "LAIUE_X86_64_TUNE должен быть generic или amd_zen4, получено: "
        "${LAIUE_X86_64_TUNE}")
endif()
option(LAIUE_ENABLE_SANITIZERS
    "Включить AddressSanitizer и UndefinedBehaviorSanitizer" OFF)

if(LAIUE_PLATFORM_EXTERNAL)
    if(NOT CMAKE_C_COMPILER_ID MATCHES "^(MSVC|GNU|Clang|AppleClang)$")
        message(FATAL_ERROR
            "The external core seam currently supports MSVC, GCC and "
            "Clang-family compilers; got ${CMAKE_C_COMPILER_ID}")
    endif()
elseif(LAIUE_PLATFORM_WINDOWS)
    if(NOT MSVC)
        message(FATAL_ERROR
            "Windows-сборка поддерживает cl.exe и clang-cl.exe")
    endif()

    # /RTC1 требует CRT, которого в сборке движка нет. CMP0184 NEW убирает
    # его из флагов, но политика действует с момента project(), а
    # CMAKE_C_FLAGS_DEBUG кэшируется на самом первом project() — у
    # суперсборки это project() родителя, где политика вправе остаться
    # OLD. Поэтому флаг снимается и явно, независимо от версии CMake:
    # движок не должен требовать, чтобы его собирали корневым проектом.
    #
    # Правка живёт в области видимости движка, а не в кэше: у родителя
    # свой CRT и свои отладочные проверки, отбирать их у него нельзя.
    # CRT-зависимая цель движка вправе вернуть флаг себе через
    # target_compile_options().
    string(REGEX REPLACE
        "(^|[ \t])[-/]RTC(su|[1suc])([ \t]|$)"
        " " CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG}")

    # CMake's MSVC Release default spells out /Ob2 after /O2. When the optional
    # aggressive profile is selected, remove that directory-scope default so
    # the target-scoped /Ob3 does not produce a D9025 override warning.
    if(CMAKE_C_COMPILER_ID STREQUAL "MSVC" AND LAIUE_AGGRESSIVE_INLINING)
        string(REGEX REPLACE
            "(^|[ \t])[-/]Ob2([ \t]|$)"
            " " _laiue_c_flags_release "${CMAKE_C_FLAGS_RELEASE}")
        set(CMAKE_C_FLAGS_RELEASE "${_laiue_c_flags_release}")
    endif()
elseif(LAIUE_PLATFORM_POSIX AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    if(NOT CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        message(FATAL_ERROR
            "Linux engine core supports GCC and Clang")
    endif()
elseif(LAIUE_PLATFORM_POSIX AND APPLE)
    if(NOT CMAKE_C_COMPILER_ID MATCHES "^(AppleClang|Clang)$")
        message(FATAL_ERROR
            "macOS engine core supports AppleClang and Clang")
    endif()
else()
    message(FATAL_ERROR
        "Поддерживаются Windows, Linux и macOS; текущая система: "
        "${CMAKE_SYSTEM_NAME}")
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "laiue поддерживает только 64-битные сборки")
endif()

set(LAIUE_TARGET_X86_64 OFF)
set(LAIUE_TARGET_ARM64 OFF)
if(APPLE AND NOT "${CMAKE_OSX_ARCHITECTURES}" STREQUAL "")
    set(_laiue_target_architectures ${CMAKE_OSX_ARCHITECTURES})
else()
    set(_laiue_target_architectures ${CMAKE_SYSTEM_PROCESSOR})
endif()

foreach(_laiue_architecture IN LISTS _laiue_target_architectures)
    if(_laiue_architecture MATCHES "^(x86_64|amd64|AMD64)$")
        set(LAIUE_TARGET_X86_64 ON)
    elseif(_laiue_architecture MATCHES "^(arm64|aarch64|ARM64|AARCH64)$")
        set(LAIUE_TARGET_ARM64 ON)
    else()
        message(FATAL_ERROR
            "Неподдерживаемая архитектура ${_laiue_architecture} для "
            "${CMAKE_SYSTEM_NAME}")
    endif()
endforeach()

if(LAIUE_TARGET_X86_64 AND LAIUE_TARGET_ARM64)
    list(LENGTH _laiue_target_architectures _laiue_architecture_count)
    if(NOT APPLE OR NOT _laiue_architecture_count EQUAL 2)
        message(FATAL_ERROR
            "Смешанная архитектурная сборка поддерживается только для "
            "macOS universal2")
    endif()
    set(LAIUE_PACKAGE_ARCHITECTURE universal2)
elseif(LAIUE_TARGET_X86_64)
    set(LAIUE_PACKAGE_ARCHITECTURE x86_64)
elseif(LAIUE_TARGET_ARM64)
    set(LAIUE_PACKAGE_ARCHITECTURE arm64)
else()
    message(FATAL_ERROR "Не удалось определить целевую архитектуру")
endif()

set(LAIUE_EXPECTED_ARCHITECTURE "auto" CACHE STRING
    "Expected target architecture: auto, x86_64, arm64 or universal2")
set_property(CACHE LAIUE_EXPECTED_ARCHITECTURE PROPERTY STRINGS
    auto x86_64 arm64 universal2)
if(NOT LAIUE_EXPECTED_ARCHITECTURE MATCHES
       "^(auto|x86_64|arm64|universal2)$")
    message(FATAL_ERROR
        "LAIUE_EXPECTED_ARCHITECTURE must be auto, x86_64, arm64 or "
        "universal2; got: ${LAIUE_EXPECTED_ARCHITECTURE}")
endif()
if(NOT LAIUE_EXPECTED_ARCHITECTURE STREQUAL "auto"
   AND NOT LAIUE_EXPECTED_ARCHITECTURE STREQUAL
           LAIUE_PACKAGE_ARCHITECTURE)
    message(FATAL_ERROR
        "Expected ${LAIUE_EXPECTED_ARCHITECTURE}, but the compiler targets "
        "${LAIUE_PACKAGE_ARCHITECTURE}")
endif()
message(STATUS
    "laiue target architecture: ${LAIUE_PACKAGE_ARCHITECTURE}")

if(LAIUE_X86_64_TUNE STREQUAL "amd_zen4"
   AND (NOT LAIUE_TARGET_X86_64 OR LAIUE_TARGET_ARM64))
    message(FATAL_ERROR
        "LAIUE_X86_64_TUNE=amd_zen4 допустим только для отдельной "
        "x86_64-сборки")
endif()

set(LAIUE_MSVC_ARCH_FLAG)
set(LAIUE_CLANG_CL_ARCH_FLAG)
set(LAIUE_POSIX_ARCH_FLAG)
if(LAIUE_TARGET_X86_64 AND NOT LAIUE_TARGET_ARM64)
    if(LAIUE_X86_64_LEVEL STREQUAL "sse2")
        set(LAIUE_MSVC_ARCH_FLAG /arch:SSE2)
        set(LAIUE_CLANG_CL_ARCH_FLAG /clang:-march=x86-64)
        set(LAIUE_POSIX_ARCH_FLAG -march=x86-64)
    elseif(LAIUE_X86_64_LEVEL STREQUAL "avx2")
        set(LAIUE_MSVC_ARCH_FLAG /arch:AVX2)
        set(LAIUE_CLANG_CL_ARCH_FLAG /clang:-march=x86-64-v3)
        set(LAIUE_POSIX_ARCH_FLAG -march=x86-64-v3)
    else()
        set(LAIUE_MSVC_ARCH_FLAG /arch:AVX512)
        set(LAIUE_CLANG_CL_ARCH_FLAG /clang:-march=x86-64-v4)
        set(LAIUE_POSIX_ARCH_FLAG -march=x86-64-v4)
    endif()
endif()

get_property(LAIUE_IS_MULTI_CONFIG GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(LAIUE_IS_MULTI_CONFIG)
    set(CMAKE_CONFIGURATION_TYPES "Debug;Release" CACHE STRING "" FORCE)
else()
    if(NOT CMAKE_BUILD_TYPE)
        set(CMAKE_BUILD_TYPE "Release" CACHE STRING "" FORCE)
    endif()
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release)
    if(NOT CMAKE_BUILD_TYPE MATCHES "^(Debug|Release)$")
        message(FATAL_ERROR
            "CMAKE_BUILD_TYPE должен быть Debug или Release, получено: "
            "${CMAKE_BUILD_TYPE}")
    endif()
endif()

# Общие правила компиляции. Все свойства намеренно target-scoped: выбор
# композиции не меняет глобальные флаги CMake и не загрязняет потребителей.
add_library(laiue_build_options INTERFACE)
add_library(laiue::build_options ALIAS laiue_build_options)
target_include_directories(laiue_build_options
    INTERFACE "${PROJECT_SOURCE_DIR}/src")
target_compile_definitions(laiue_build_options INTERFACE
    "LAIUE_VERSION_TEXT=L\"${PROJECT_VERSION}\""
    LAIUE_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    LAIUE_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    LAIUE_VERSION_PATCH=${PROJECT_VERSION_PATCH}
)

if(LAIUE_PLATFORM_WINDOWS)
    target_compile_definitions(laiue_build_options INTERFACE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        UNICODE
        _UNICODE
    )
    target_compile_options(laiue_build_options INTERFACE
        /W4 /utf-8 /GS-
        $<$<BOOL:${LAIUE_WARNINGS_AS_ERRORS}>:/WX>
        $<$<CONFIG:Debug>:/Od>
        $<$<CONFIG:Release>:/O2 /Ot /Oi /GF /Gy /Gw /volatile:iso>
        $<$<AND:$<C_COMPILER_ID:MSVC>,$<CONFIG:Release>>:/Zc:inline ${LAIUE_MSVC_ARCH_FLAG}>
        $<$<AND:$<C_COMPILER_ID:Clang>,$<CONFIG:Release>>:/Qvec ${LAIUE_CLANG_CL_ARCH_FLAG} /clang:-O3 /clang:-fvectorize /clang:-fslp-vectorize /clang:-fno-math-errno>
        $<$<AND:$<C_COMPILER_ID:MSVC>,$<CONFIG:Release>,$<BOOL:${LAIUE_AGGRESSIVE_INLINING}>>:/Ob3>
        $<$<C_COMPILER_ID:Clang>:-Wno-unused-command-line-argument>
    )
    if(LAIUE_X86_64_TUNE STREQUAL "amd_zen4")
        target_compile_options(laiue_build_options INTERFACE
            $<$<AND:$<C_COMPILER_ID:MSVC>,$<CONFIG:Release>>:/favor:AMD64>
            $<$<AND:$<C_COMPILER_ID:Clang>,$<CONFIG:Release>>:/clang:-mtune=znver4>)
    endif()
elseif(MSVC)
    # Licensed Win32-family targets (for example GDKX) own their runtime and
    # SDK policy in the external adapter. Keep the compiler optimizations but
    # never inherit the desktop /NODEFAULTLIB contract implicitly.
    target_compile_definitions(laiue_build_options INTERFACE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        UNICODE
        _UNICODE)
    target_compile_options(laiue_build_options INTERFACE
        /W4 /utf-8
        $<$<BOOL:${LAIUE_WARNINGS_AS_ERRORS}>:/WX>
        $<$<CONFIG:Debug>:/Od>
        $<$<CONFIG:Release>:/O2 /Ot /Oi /GF /Gy /Gw /volatile:iso>
        $<$<AND:$<C_COMPILER_ID:MSVC>,$<CONFIG:Release>>:/Zc:inline ${LAIUE_MSVC_ARCH_FLAG}>
        $<$<AND:$<C_COMPILER_ID:Clang>,$<CONFIG:Release>>:/Qvec ${LAIUE_CLANG_CL_ARCH_FLAG} /clang:-O3 /clang:-fvectorize /clang:-fslp-vectorize /clang:-fno-math-errno>
        $<$<AND:$<C_COMPILER_ID:MSVC>,$<CONFIG:Release>,$<BOOL:${LAIUE_AGGRESSIVE_INLINING}>>:/Ob3>
        $<$<C_COMPILER_ID:Clang>:-Wno-unused-command-line-argument>)
else()
    target_compile_options(laiue_build_options INTERFACE
        -Wall -Wextra -Wpedantic
        $<$<BOOL:${LAIUE_WARNINGS_AS_ERRORS}>:-Werror>
        $<$<CONFIG:Debug>:-O0;-g3>
        $<$<CONFIG:Release>:-O3;-fno-math-errno;${LAIUE_POSIX_ARCH_FLAG}>
    )
    if(LAIUE_X86_64_TUNE STREQUAL "amd_zen4")
        target_compile_options(laiue_build_options INTERFACE
            $<$<CONFIG:Release>:-mtune=znver4>)
    endif()
    if(LAIUE_PLATFORM_POSIX AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
        target_compile_options(laiue_build_options INTERFACE
            $<$<CONFIG:Release>:-fno-plt;-fno-semantic-interposition;-ffunction-sections;-fdata-sections>)
        target_link_options(laiue_build_options INTERFACE
            $<$<CONFIG:Release>:-Wl,-O1,--gc-sections,--as-needed,-Bsymbolic-functions>)

        set(LAIUE_LINUX_LIBC "gnu" CACHE STRING
            "Linux libc ABI for engine artifacts: gnu or musl")
        set_property(CACHE LAIUE_LINUX_LIBC PROPERTY STRINGS gnu musl)
        include(CheckCSourceCompiles)
        check_c_source_compiles(
            "#include <features.h>
             #ifndef __GLIBC__
             #error not glibc
             #endif
             int main(void) { return 0; }"
            LAIUE_COMPILER_USES_GLIBC)
        if(LAIUE_LINUX_LIBC STREQUAL "gnu")
            if(NOT LAIUE_COMPILER_USES_GLIBC)
                message(FATAL_ERROR
                    "LAIUE_LINUX_LIBC=gnu требует glibc toolchain")
            endif()
            target_compile_definitions(laiue_build_options INTERFACE
                LAIUE_LINUX_LIBC_GNU=1)
        elseif(LAIUE_LINUX_LIBC STREQUAL "musl")
            if(LAIUE_COMPILER_USES_GLIBC)
                message(FATAL_ERROR
                    "LAIUE_LINUX_LIBC=musl нельзя собирать glibc compiler; "
                    "используйте native Alpine или musl toolchain")
            endif()
            target_compile_definitions(laiue_build_options INTERFACE
                LAIUE_LINUX_LIBC_MUSL=1)
        else()
            message(FATAL_ERROR
                "LAIUE_LINUX_LIBC должен быть gnu или musl, получено: "
                "${LAIUE_LINUX_LIBC}")
        endif()
    elseif(LAIUE_PLATFORM_POSIX AND APPLE)
        target_link_options(laiue_build_options INTERFACE
            $<$<CONFIG:Release>:-Wl,-dead_strip>)
    elseif(LAIUE_PLATFORM_EXTERNAL AND
           CMAKE_SYSTEM_NAME STREQUAL "Android")
        target_compile_options(laiue_build_options INTERFACE
            $<$<CONFIG:Release>:-ffunction-sections;-fdata-sections;-fno-semantic-interposition>)
        target_link_options(laiue_build_options INTERFACE
            $<$<CONFIG:Release>:-Wl,-O2,--gc-sections,--as-needed,-Bsymbolic-functions>)
    elseif(LAIUE_PLATFORM_EXTERNAL AND APPLE)
        target_link_options(laiue_build_options INTERFACE
            $<$<CONFIG:Release>:-Wl,-dead_strip>)
    endif()

    if(LAIUE_ENABLE_SANITIZERS)
        target_compile_options(laiue_build_options INTERFACE
            -fno-omit-frame-pointer -fsanitize=address,undefined)
        target_link_options(laiue_build_options INTERFACE
            -fsanitize=address,undefined)
    endif()
endif()

# Windows no-CRT является отдельным opt-in контрактом. Linux-цели никогда не
# наследуют /NODEFAULTLIB или собственные memcpy/memset.
add_library(laiue_windows_no_crt INTERFACE)
add_library(laiue::windows_no_crt ALIAS laiue_windows_no_crt)
if(LAIUE_PLATFORM_WINDOWS)
    add_library(laiue_runtime OBJECT
        "${PROJECT_SOURCE_DIR}/src/runtime/memory.c"
        "${PROJECT_SOURCE_DIR}/src/runtime/wide_string.c")
    target_compile_options(laiue_runtime PRIVATE
        /W4 /utf-8 /GS-
        $<$<BOOL:${LAIUE_WARNINGS_AS_ERRORS}>:/WX>
        $<$<CONFIG:Debug>:/Od>
        $<$<CONFIG:Release>:/O2 /Ot /Oi /GF /Gy /Gw /volatile:iso>
        $<$<AND:$<C_COMPILER_ID:MSVC>,$<CONFIG:Release>>:/Zc:inline ${LAIUE_MSVC_ARCH_FLAG}>
        $<$<AND:$<C_COMPILER_ID:Clang>,$<CONFIG:Release>>:/Qvec ${LAIUE_CLANG_CL_ARCH_FLAG} /clang:-O3 /clang:-fvectorize /clang:-fslp-vectorize /clang:-fno-math-errno>)
    if(LAIUE_X86_64_TUNE STREQUAL "amd_zen4")
        target_compile_options(laiue_runtime PRIVATE
            $<$<AND:$<C_COMPILER_ID:MSVC>,$<CONFIG:Release>>:/favor:AMD64>
            $<$<AND:$<C_COMPILER_ID:Clang>,$<CONFIG:Release>>:/clang:-mtune=znver4>)
    endif()
    target_sources(laiue_windows_no_crt INTERFACE
        "$<TARGET_OBJECTS:laiue_runtime>")
    if(LAIUE_TARGET_ARM64)
        # winnt.h просит развернуть _Interlocked* только в ветках _M_AMD64 и
        # _M_IX86, поэтому на ARM64 при /Od MSVC оставляет вызовы помощников;
        # ни /Oi, ни собственная #pragma intrinsic этого не меняют. arm64rt.lib
        # из Windows SDK — их реализация: нужные члены не определяют memset,
        # memcpy и прочий CRT и линкуются вообще без импортов. clang-cl
        # разворачивает интринсики сам, и библиотека ему не нужна.
        target_link_libraries(laiue_windows_no_crt INTERFACE
            $<$<C_COMPILER_ID:MSVC>:arm64rt.lib>)
    endif()
    target_link_options(laiue_windows_no_crt INTERFACE
        /NODEFAULTLIB
        /DYNAMICBASE /HIGHENTROPYVA /NXCOMPAT
        /MANIFEST:NO
        /MERGE:.rdata=.text /MERGE:.pdata=.text
        $<$<C_COMPILER_ID:MSVC>:/EMITTOOLVERSIONINFO:NO>
        $<$<CONFIG:Debug>:/DEBUG /INCREMENTAL:NO>
        $<$<CONFIG:Release>:/INCREMENTAL:NO /OPT:REF /OPT:ICF=10>
    )
else()
    # Совместимое имя избавляет старые локальные CMake-потребители от
    # платформенных if(); объектного no-CRT runtime на Linux нет.
    add_library(laiue_runtime INTERFACE)
endif()

if(LAIUE_ENABLE_LTO)
    if(LAIUE_PLATFORM_WINDOWS OR MSVC)
        target_compile_options(laiue_build_options INTERFACE
            $<$<AND:$<C_COMPILER_ID:MSVC>,$<CONFIG:Release>>:/GL>
            $<$<AND:$<C_COMPILER_ID:Clang>,$<CONFIG:Release>>:-flto=${LAIUE_CLANG_LTO_MODE}>)
        target_link_options(laiue_build_options INTERFACE
            $<$<AND:$<C_COMPILER_ID:MSVC>,$<CONFIG:Release>>:/LTCG>
            $<$<AND:$<C_COMPILER_ID:Clang>,$<CONFIG:Release>>:/OPT:LLDLTO=3 /OPT:LLDLTOCGO=3>)
    elseif(LAIUE_PLATFORM_POSIX AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
        target_compile_options(laiue_build_options INTERFACE
            $<$<AND:$<C_COMPILER_ID:GNU>,$<CONFIG:Release>>:-flto>
            $<$<AND:$<C_COMPILER_ID:Clang>,$<CONFIG:Release>>:-flto=full>)
        target_link_options(laiue_build_options INTERFACE
            $<$<AND:$<C_COMPILER_ID:GNU>,$<CONFIG:Release>>:-flto;-flto-partition=one>
            $<$<AND:$<C_COMPILER_ID:Clang>,$<CONFIG:Release>>:-flto=full>)
    elseif(LAIUE_PLATFORM_POSIX AND APPLE)
        target_compile_options(laiue_build_options INTERFACE
            $<$<AND:$<C_COMPILER_ID:AppleClang,Clang>,$<CONFIG:Release>>:-flto=full>)
        target_link_options(laiue_build_options INTERFACE
            $<$<AND:$<C_COMPILER_ID:AppleClang,Clang>,$<CONFIG:Release>>:-flto=full>)
    elseif(LAIUE_PLATFORM_EXTERNAL AND
           CMAKE_SYSTEM_NAME MATCHES "^(Android|iOS|tvOS|visionOS)$")
        target_compile_options(laiue_build_options INTERFACE
            $<$<AND:$<C_COMPILER_ID:AppleClang,Clang>,$<CONFIG:Release>>:-flto=full>)
        target_link_options(laiue_build_options INTERFACE
            $<$<AND:$<C_COMPILER_ID:AppleClang,Clang>,$<CONFIG:Release>>:-flto=full>)
    elseif(LAIUE_PLATFORM_EXTERNAL)
        target_compile_options(laiue_build_options INTERFACE
            $<$<AND:$<C_COMPILER_ID:GNU>,$<CONFIG:Release>>:-flto>
            $<$<AND:$<C_COMPILER_ID:AppleClang,Clang>,$<CONFIG:Release>>:-flto=full>)
        target_link_options(laiue_build_options INTERFACE
            $<$<AND:$<C_COMPILER_ID:GNU>,$<CONFIG:Release>>:-flto;-flto-partition=one>
            $<$<AND:$<C_COMPILER_ID:AppleClang,Clang>,$<CONFIG:Release>>:-flto=full>)
    endif()
endif()

# Internal compatibility aggregate used by engine modules and tests.
add_library(laiue_common INTERFACE)
target_link_libraries(laiue_common INTERFACE laiue::build_options)
if(LAIUE_PLATFORM_WINDOWS)
    target_link_libraries(laiue_common INTERFACE laiue::windows_no_crt)
endif()
