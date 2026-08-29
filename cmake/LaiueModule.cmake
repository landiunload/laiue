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

function(laiue_register_runtime_target target_name runtime_role)
    if(NOT TARGET "${target_name}")
        message(FATAL_ERROR
            "Нельзя зарегистрировать отсутствующую цель ${target_name}")
    endif()
    string(TOUPPER "${runtime_role}" normalized_role)
    if(NOT normalized_role MATCHES "^(CORE|GRAPHICS)$")
        message(FATAL_ERROR
            "Цель ${target_name} имеет неизвестную runtime role: "
            "${runtime_role}")
    endif()
    set_property(GLOBAL APPEND PROPERTY LAIUE_RUNTIME_TARGETS "${target_name}")
    set_property(GLOBAL APPEND PROPERTY
        "LAIUE_RUNTIME_${normalized_role}_TARGETS" "${target_name}")
endfunction()

function(laiue_add_module module_name)
    cmake_parse_arguments(PARSE_ARGV 1 MODULE
        "PRECISE_FP"
        "RUNTIME_ROLE"
        "SOURCES;WINDOWS_SOURCES;UNIX_SOURCES;LINK;PUBLIC_LINK;WINDOWS_LINK;UNIX_LINK"
    )

    set(selected_sources ${MODULE_SOURCES})
    set(selected_links ${MODULE_LINK})
    if(WIN32)
        list(APPEND selected_sources ${MODULE_WINDOWS_SOURCES})
        list(APPEND selected_links ${MODULE_WINDOWS_LINK})
    else()
        list(APPEND selected_sources ${MODULE_UNIX_SOURCES})
        list(APPEND selected_links ${MODULE_UNIX_LINK})
    endif()
    if(NOT selected_sources)
        message(FATAL_ERROR "laiue_add_module(${module_name}) требует SOURCES")
    endif()
    if(NOT MODULE_RUNTIME_ROLE)
        message(FATAL_ERROR
            "laiue_add_module(${module_name}) требует RUNTIME_ROLE")
    endif()

    laiue_collect_sources(module_sources ${selected_sources})
    set(target_name "laiue_${module_name}")
    string(TOUPPER "${module_name}" module_name_upper)

    add_library(${target_name} SHARED)
    add_library("laiue::${module_name}" ALIAS ${target_name})
    target_sources(${target_name} PRIVATE ${module_sources})
    source_group(TREE "${PROJECT_SOURCE_DIR}" FILES ${module_sources})
    target_compile_definitions(${target_name}
        PRIVATE "LAIUE_BUILD_${module_name_upper}")
    if(WIN32)
        if(MODULE_PRECISE_FP)
            target_compile_options(${target_name} PRIVATE
                "$<$<C_COMPILER_ID:MSVC>:/fp:strict>"
                "$<$<C_COMPILER_ID:Clang>:/clang:-ffp-model=strict>")
        else()
            target_compile_options(${target_name} PRIVATE /fp:fast)
        endif()
    elseif(MODULE_PRECISE_FP)
        target_compile_options(${target_name} PRIVATE
            -fno-fast-math
            -ffp-contract=off)
    endif()
    target_include_directories(${target_name}
        PUBLIC
            "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>"
            "$<INSTALL_INTERFACE:include/laiue>")
    target_link_libraries(${target_name}
        PRIVATE
            laiue_common
            laiue::platform_support
            ${selected_links})
    if(MODULE_PUBLIC_LINK)
        target_link_libraries(${target_name} PUBLIC ${MODULE_PUBLIC_LINK})
    endif()

    if(WIN32)
        target_link_options(${target_name} PRIVATE /NOENTRY)
    else()
        set_target_properties(${target_name} PROPERTIES
            C_VISIBILITY_PRESET hidden
            VISIBILITY_INLINES_HIDDEN YES
            BUILD_RPATH "$ORIGIN"
            INSTALL_RPATH "$ORIGIN")
    endif()

    laiue_register_runtime_target(
        ${target_name} "${MODULE_RUNTIME_ROLE}")
    set_target_properties(${target_name} PROPERTIES
        EXPORT_NAME "${module_name}")
endfunction()
