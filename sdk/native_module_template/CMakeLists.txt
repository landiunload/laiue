cmake_minimum_required(VERSION 3.20)
project(laiue_example_service LANGUAGES C)

set(LAIUE_SDK_INCLUDE_DIR "" CACHE PATH "Installed LAIUE include directory")
if(LAIUE_SDK_INCLUDE_DIR STREQUAL "")
    message(FATAL_ERROR "Set LAIUE_SDK_INCLUDE_DIR to the installed LAIUE include directory")
endif()

function(add_example_module target source)
    add_library("${target}" SHARED "${source}")
    target_include_directories("${target}" PRIVATE "${LAIUE_SDK_INCLUDE_DIR}")
    set_target_properties("${target}" PROPERTIES
        C_STANDARD 17
        C_STANDARD_REQUIRED ON)
    if(NOT WIN32)
        set_target_properties("${target}" PROPERTIES
            C_VISIBILITY_PRESET hidden
            VISIBILITY_INLINES_HIDDEN YES)
    endif()
endfunction()

add_example_module(example_service module.c)
add_example_module(example_ui_extension ui_extension.c)
add_example_module(example_material_shader material_shader.c)

# There is intentionally no target_link_libraries() call: a native module
# talks to LAIUE only through the standalone C ABI table.
