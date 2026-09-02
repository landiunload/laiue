include_guard(GLOBAL)

set(LAIUE_PLATFORM_BACKEND "AUTO" CACHE STRING
    "Platform backend: AUTO, WINDOWS, POSIX or EXTERNAL")
set_property(CACHE LAIUE_PLATFORM_BACKEND PROPERTY STRINGS
    AUTO WINDOWS POSIX EXTERNAL)
string(TOUPPER "${LAIUE_PLATFORM_BACKEND}" _laiue_platform_backend)

if(_laiue_platform_backend STREQUAL "AUTO")
    if(WIN32)
        set(_laiue_platform_backend WINDOWS)
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux" OR
           CMAKE_SYSTEM_NAME STREQUAL "Darwin")
        set(_laiue_platform_backend POSIX)
    else()
        message(FATAL_ERROR
            "No built-in platform backend exists for ${CMAKE_SYSTEM_NAME}; "
            "set LAIUE_PLATFORM_BACKEND=EXTERNAL and provide an adapter file")
    endif()
endif()
if(NOT _laiue_platform_backend MATCHES "^(WINDOWS|POSIX|EXTERNAL)$")
    message(FATAL_ERROR
        "LAIUE_PLATFORM_BACKEND must be AUTO, WINDOWS, POSIX or EXTERNAL; "
        "got: ${LAIUE_PLATFORM_BACKEND}")
endif()

set(LAIUE_PLATFORM_WINDOWS OFF)
set(LAIUE_PLATFORM_POSIX OFF)
set(LAIUE_PLATFORM_EXTERNAL OFF)
if(_laiue_platform_backend STREQUAL "WINDOWS")
    if(NOT WIN32)
        message(FATAL_ERROR "The WINDOWS backend requires a Windows target")
    endif()
    set(LAIUE_PLATFORM_WINDOWS ON)
elseif(_laiue_platform_backend STREQUAL "POSIX")
    if(NOT (CMAKE_SYSTEM_NAME STREQUAL "Linux" OR
            CMAKE_SYSTEM_NAME STREQUAL "Darwin"))
        message(FATAL_ERROR
            "The built-in POSIX backend supports Linux and macOS only")
    endif()
    set(LAIUE_PLATFORM_POSIX ON)
else()
    set(LAIUE_PLATFORM_EXTERNAL ON)
endif()

set(LAIUE_EXTERNAL_PLATFORM_FILE "" CACHE FILEPATH
    "CMake adapter file used by the EXTERNAL platform backend")
set(LAIUE_PLATFORM_ADAPTER_TARGET "" CACHE STRING
    "Target implementing src/platform/system.h for an external backend")
set(LAIUE_PRECISE_FP_TARGET "" CACHE STRING
    "External interface target providing deterministic strict-FP flags")
set(LAIUE_ADAPTER_USE_ENGINE_BUILD_OPTIONS OFF)

if(LAIUE_PLATFORM_EXTERNAL)
    if(LAIUE_EXTERNAL_PLATFORM_FILE STREQUAL "" OR
       NOT EXISTS "${LAIUE_EXTERNAL_PLATFORM_FILE}")
        message(FATAL_ERROR
            "EXTERNAL requires an existing LAIUE_EXTERNAL_PLATFORM_FILE")
    endif()
    include("${LAIUE_EXTERNAL_PLATFORM_FILE}")
    if(LAIUE_PLATFORM_ADAPTER_TARGET STREQUAL "" OR
       NOT TARGET "${LAIUE_PLATFORM_ADAPTER_TARGET}")
        message(FATAL_ERROR
            "The external adapter file must create and name "
            "LAIUE_PLATFORM_ADAPTER_TARGET")
    endif()
    if(LAIUE_PRECISE_FP_TARGET STREQUAL "" OR
       NOT TARGET "${LAIUE_PRECISE_FP_TARGET}")
        message(FATAL_ERROR
            "The external adapter file must create and name "
            "LAIUE_PRECISE_FP_TARGET")
    endif()
endif()

set(LAIUE_MODULE_LIBRARY_TYPE "AUTO" CACHE STRING
    "Engine module type: AUTO, SHARED or STATIC")
set_property(CACHE LAIUE_MODULE_LIBRARY_TYPE PROPERTY STRINGS
    AUTO SHARED STATIC)
string(TOUPPER "${LAIUE_MODULE_LIBRARY_TYPE}"
    LAIUE_MODULE_LIBRARY_TYPE_RESOLVED)
if(LAIUE_MODULE_LIBRARY_TYPE_RESOLVED STREQUAL "AUTO")
    if(LAIUE_PLATFORM_EXTERNAL)
        set(LAIUE_MODULE_LIBRARY_TYPE_RESOLVED STATIC)
    else()
        set(LAIUE_MODULE_LIBRARY_TYPE_RESOLVED SHARED)
    endif()
endif()
if(NOT LAIUE_MODULE_LIBRARY_TYPE_RESOLVED MATCHES "^(SHARED|STATIC)$")
    message(FATAL_ERROR
        "LAIUE_MODULE_LIBRARY_TYPE must be AUTO, SHARED or STATIC")
endif()
if(LAIUE_PLATFORM_EXTERNAL AND
   NOT LAIUE_MODULE_LIBRARY_TYPE_RESOLVED STREQUAL "STATIC")
    message(FATAL_ERROR "The initial EXTERNAL core contract requires STATIC modules")
endif()
if(NOT LAIUE_PLATFORM_EXTERNAL AND
   NOT LAIUE_MODULE_LIBRARY_TYPE_RESOLVED STREQUAL "SHARED")
    message(FATAL_ERROR "Built-in desktop backends currently require SHARED modules")
endif()

set(LAIUE_NATIVE_MOD_MODE "AUTO" CACHE STRING
    "Native code mods: AUTO, DYNAMIC or OFF")
set_property(CACHE LAIUE_NATIVE_MOD_MODE PROPERTY STRINGS AUTO DYNAMIC OFF)
string(TOUPPER "${LAIUE_NATIVE_MOD_MODE}" LAIUE_NATIVE_MOD_MODE_RESOLVED)
if(LAIUE_NATIVE_MOD_MODE_RESOLVED STREQUAL "AUTO")
    if(LAIUE_PLATFORM_EXTERNAL)
        set(LAIUE_NATIVE_MOD_MODE_RESOLVED OFF)
    else()
        set(LAIUE_NATIVE_MOD_MODE_RESOLVED DYNAMIC)
    endif()
endif()
if(NOT LAIUE_NATIVE_MOD_MODE_RESOLVED MATCHES "^(DYNAMIC|OFF)$")
    message(FATAL_ERROR "LAIUE_NATIVE_MOD_MODE must be AUTO, DYNAMIC or OFF")
endif()
if(LAIUE_PLATFORM_EXTERNAL AND
   NOT LAIUE_NATIVE_MOD_MODE_RESOLVED STREQUAL "OFF")
    message(FATAL_ERROR
        "The initial EXTERNAL contract disables dynamically loaded native mods")
endif()

set(LAIUE_ENABLE_SDK_INSTALL "AUTO" CACHE STRING
    "Install/export SDK artifacts: AUTO, ON or OFF")
set_property(CACHE LAIUE_ENABLE_SDK_INSTALL PROPERTY STRINGS AUTO ON OFF)
string(TOUPPER "${LAIUE_ENABLE_SDK_INSTALL}"
    LAIUE_ENABLE_SDK_INSTALL_RESOLVED)
if(LAIUE_ENABLE_SDK_INSTALL_RESOLVED STREQUAL "AUTO")
    if(LAIUE_PLATFORM_EXTERNAL)
        set(LAIUE_ENABLE_SDK_INSTALL_RESOLVED OFF)
    else()
        set(LAIUE_ENABLE_SDK_INSTALL_RESOLVED ON)
    endif()
endif()
if(NOT LAIUE_ENABLE_SDK_INSTALL_RESOLVED MATCHES "^(ON|OFF)$")
    message(FATAL_ERROR "LAIUE_ENABLE_SDK_INSTALL must be AUTO, ON or OFF")
endif()
if(LAIUE_PLATFORM_EXTERNAL AND LAIUE_ENABLE_SDK_INSTALL_RESOLVED)
    message(FATAL_ERROR
        "External ports are integrated by a parent superbuild; SDK export is disabled")
endif()

set(LAIUE_NATIVE_MODS_ENABLED OFF)
if(LAIUE_NATIVE_MOD_MODE_RESOLVED STREQUAL "DYNAMIC")
    set(LAIUE_NATIVE_MODS_ENABLED ON)
endif()

message(STATUS
    "laiue platform backend: ${_laiue_platform_backend}; modules: "
    "${LAIUE_MODULE_LIBRARY_TYPE_RESOLVED}; native mods: "
    "${LAIUE_NATIVE_MOD_MODE_RESOLVED}")
