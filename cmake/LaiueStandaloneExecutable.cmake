include_guard(GLOBAL)

# Standalone targets use the engine's /NODEFAULTLIB profile on Windows, so
# they need an explicit entry point. Console tools are the default; windowed
# applications can request the Windows subsystem as the optional third arg.
function(laiue_configure_standalone_executable target_name windows_entry)
    target_link_libraries(${target_name} PRIVATE
        laiue_common
        laiue::platform_support)
    if(WIN32)
        target_link_libraries(${target_name} PRIVATE kernel32)
        target_link_options(${target_name} PRIVATE "/ENTRY:${windows_entry}")
        if(ARGC GREATER 2 AND ARGV2 STREQUAL "WINDOWED")
            target_link_options(${target_name} PRIVATE /SUBSYSTEM:WINDOWS)
        else()
            target_link_options(${target_name} PRIVATE /SUBSYSTEM:CONSOLE)
        endif()
    endif()
endfunction()

# Adds a console test or benchmark built like the engine:
#
#   laiue_add_harness(<target> <entry> <sources>...
#       [TEST <name>] [TIMEOUT <seconds>] [PROPERTIES <test properties>...]
#       [BOOTSTRAP] [STATIC] [PRECISE_FP]
#       [LIBRARIES <libraries>...] [INCLUDES <directories>...]
#       [DEFINITIONS <definitions>...] [DEPENDS <targets>...])
#
# TEST registers the executable in CTest; without it the target stays out of
# ALL and is built on request. BOOTSTRAP links the static module host, and
# STATIC removes dllimport from engine sources compiled into the harness.
function(laiue_add_harness target_name windows_entry)
    cmake_parse_arguments(PARSE_ARGV 2 arg "BOOTSTRAP;STATIC;PRECISE_FP"
        "TEST;TIMEOUT" "LIBRARIES;INCLUDES;DEFINITIONS;DEPENDS;PROPERTIES")
    if(arg_TEST)
        add_executable(${target_name} ${arg_UNPARSED_ARGUMENTS})
    else()
        add_executable(${target_name} EXCLUDE_FROM_ALL ${arg_UNPARSED_ARGUMENTS})
    endif()
    if(arg_BOOTSTRAP)
        list(PREPEND arg_LIBRARIES laiue_bootstrap)
    endif()
    if(arg_BOOTSTRAP OR arg_STATIC)
        list(APPEND arg_DEFINITIONS LAIUE_STATIC=1)
    endif()
    target_link_libraries(${target_name} PRIVATE ${arg_LIBRARIES})
    target_include_directories(${target_name} PRIVATE
        "${PROJECT_SOURCE_DIR}/tests" ${arg_INCLUDES})
    target_compile_definitions(${target_name} PRIVATE ${arg_DEFINITIONS})
    if(arg_DEPENDS)
        add_dependencies(${target_name} ${arg_DEPENDS})
    endif()
    if(arg_PRECISE_FP)
        laiue_configure_precise_fp(${target_name})
    endif()
    laiue_configure_standalone_executable(${target_name} ${windows_entry})
    if(arg_TEST)
        add_test(NAME ${arg_TEST} COMMAND ${target_name})
        if(arg_TIMEOUT)
            list(APPEND arg_PROPERTIES TIMEOUT ${arg_TIMEOUT})
        endif()
        if(arg_PROPERTIES)
            set_tests_properties(${arg_TEST} PROPERTIES ${arg_PROPERTIES})
        endif()
    endif()
endfunction()
