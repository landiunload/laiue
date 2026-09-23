cmake_minimum_required(VERSION 3.20)
project(laiue_example_service LANGUAGES C)

set(LAIUE_SDK_INCLUDE_DIR "" CACHE PATH "Installed LAIUE include directory")
if(LAIUE_SDK_INCLUDE_DIR STREQUAL "")
    message(FATAL_ERROR "Set LAIUE_SDK_INCLUDE_DIR to the installed LAIUE include directory")
endif()

add_library(example_service SHARED module.c)
target_include_directories(example_service PRIVATE "${LAIUE_SDK_INCLUDE_DIR}")
set_target_properties(example_service PROPERTIES
    C_STANDARD 17
    C_STANDARD_REQUIRED ON)
if(NOT WIN32)
    set_target_properties(example_service PROPERTIES
        C_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN YES)
endif()

# There is intentionally no target_link_libraries() call: a native module
# talks to LAIUE only through the standalone C ABI table.
