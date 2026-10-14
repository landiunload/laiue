if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "The public external-port mock is a Linux CI fixture")
endif()

find_package(Threads REQUIRED)

set(_laiue_external_mock_root "${CMAKE_CURRENT_LIST_DIR}/../..")
add_library(laiue_external_mock_adapter STATIC
    "${_laiue_external_mock_root}/src/platform/system_posix.c"
    "${_laiue_external_mock_root}/src/platform/sha256.c"
    "${_laiue_external_mock_root}/src/platform/sha256.h"
    "${_laiue_external_mock_root}/src/platform/system.h")
target_compile_features(laiue_external_mock_adapter PRIVATE c_std_17)
target_include_directories(laiue_external_mock_adapter PRIVATE
    "${_laiue_external_mock_root}/src")
target_link_libraries(laiue_external_mock_adapter PUBLIC
    Threads::Threads ${CMAKE_DL_LIBS})

add_library(laiue_external_mock_precise_fp INTERFACE)
target_compile_options(laiue_external_mock_precise_fp INTERFACE
    -fno-fast-math
    -ffp-contract=off)

set(LAIUE_PLATFORM_ADAPTER_TARGET laiue_external_mock_adapter)
set(LAIUE_PRECISE_FP_TARGET laiue_external_mock_precise_fp)
set(LAIUE_ADAPTER_USE_ENGINE_BUILD_OPTIONS ON)
