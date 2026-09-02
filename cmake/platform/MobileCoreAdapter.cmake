if(NOT (CMAKE_SYSTEM_NAME STREQUAL "Android" OR
        CMAKE_SYSTEM_NAME STREQUAL "iOS" OR
        CMAKE_SYSTEM_NAME STREQUAL "tvOS" OR
        CMAKE_SYSTEM_NAME STREQUAL "visionOS"))
    message(FATAL_ERROR
        "MobileCoreAdapter supports Android and Apple mobile-family targets")
endif()

set(_laiue_mobile_adapter_root "${CMAKE_CURRENT_LIST_DIR}/../..")
add_library(laiue_mobile_core_adapter STATIC
    "${_laiue_mobile_adapter_root}/src/platform/system_posix.c"
    "${_laiue_mobile_adapter_root}/src/platform/sha256.c"
    "${_laiue_mobile_adapter_root}/src/platform/sha256.h"
    "${_laiue_mobile_adapter_root}/src/platform/system.h")
target_compile_features(laiue_mobile_core_adapter PRIVATE c_std_17)
target_include_directories(laiue_mobile_core_adapter PRIVATE
    "${_laiue_mobile_adapter_root}/src")
target_compile_definitions(laiue_mobile_core_adapter PRIVATE
    LAIUE_MOBILE_PLATFORM=1)
set_target_properties(laiue_mobile_core_adapter PROPERTIES
    C_VISIBILITY_PRESET hidden
    POSITION_INDEPENDENT_CODE ON)

if(APPLE)
    find_library(LAIUE_MOBILE_SECURITY_FRAMEWORK Security REQUIRED)
    target_link_libraries(laiue_mobile_core_adapter PUBLIC
        "${LAIUE_MOBILE_SECURITY_FRAMEWORK}")
elseif(CMAKE_DL_LIBS)
    target_link_libraries(laiue_mobile_core_adapter PUBLIC
        ${CMAKE_DL_LIBS})
endif()

add_library(laiue_mobile_precise_fp INTERFACE)
target_compile_options(laiue_mobile_precise_fp INTERFACE
    -fno-fast-math
    -ffp-contract=off)

set(LAIUE_PLATFORM_ADAPTER_TARGET laiue_mobile_core_adapter)
set(LAIUE_PRECISE_FP_TARGET laiue_mobile_precise_fp)
set(LAIUE_ADAPTER_USE_ENGINE_BUILD_OPTIONS ON)
