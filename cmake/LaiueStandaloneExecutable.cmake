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
