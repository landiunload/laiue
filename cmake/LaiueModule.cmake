include_guard(GLOBAL)

function(laiue_collect_sources out_variable)
    set(result)
    foreach(source IN LISTS ARGN)
        if(IS_ABSOLUTE "${source}")
            list(APPEND result "${source}")
        else()
            list(APPEND result "${CMAKE_CURRENT_SOURCE_DIR}/${source}")
        endif()
    endforeach()
    set(${out_variable} "${result}" PARENT_SCOPE)
endfunction()

function(laiue_add_module module_name)
    cmake_parse_arguments(PARSE_ARGV 1 MODULE
        ""
        ""
        "SOURCES;LINK;PUBLIC_LINK"
    )

    if(NOT MODULE_SOURCES)
        message(FATAL_ERROR "laiue_add_module(${module_name}) требует SOURCES")
    endif()

    laiue_collect_sources(module_sources ${MODULE_SOURCES})
    set(target_name "laiue_${module_name}")
    string(TOUPPER "${module_name}" module_name_upper)

    add_library(${target_name} SHARED)
    target_sources(${target_name} PRIVATE ${module_sources})
    source_group(TREE "${PROJECT_SOURCE_DIR}" FILES ${module_sources})

    target_compile_definitions(${target_name}
        PRIVATE "LAIUE_BUILD_${module_name_upper}"
    )
    target_link_libraries(${target_name}
        PRIVATE
            laiue_common
            laiue_runtime
            kernel32
            ${MODULE_LINK}
    )
    if(MODULE_PUBLIC_LINK)
        target_link_libraries(${target_name} PUBLIC ${MODULE_PUBLIC_LINK})
    endif()
    target_link_options(${target_name} PRIVATE /NOENTRY)
endfunction()
