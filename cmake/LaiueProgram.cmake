include_guard(GLOBAL)

# laiue_add_program(<name> <sources>...
#     [MODULES <имена модулей>...]
#     [WINDOWED]
#     [ANDROID_PACKAGE <идентификатор>] [ANDROID_LABEL <название>]
#     [ANDROID_ORIENTATION <screenOrientation>])
#
# Программа на laiue — обычный main на C: она линкует только bootstrap и
# грузит модули по имени, поэтому одна и та же цель собирается под любую
# систему. Под Windows main получает аргументы в UTF-8 от начала программы
# без CRT, WINDOWED убирает консоль. Под Android программа — разделяемая
# библиотека, а цель <name>_apk собирает APK, где модули лежат отдельными
# .so: модуль окна запускает её main. MODULES собираются вместе с
# программой; отсутствующий в сборке модуль пропускается, и программа
# работает без его технологии.
function(laiue_add_program name)
    cmake_parse_arguments(PARSE_ARGV 1 program "WINDOWED"
        "ANDROID_PACKAGE;ANDROID_LABEL;ANDROID_ORIENTATION" "MODULES")
    if(NOT LAIUE_MODULE_LIBRARY_TYPE_RESOLVED STREQUAL "SHARED")
        message(FATAL_ERROR "laiue_add_program(${name}) loads modules by name and needs "
            "LAIUE_MODULE_LIBRARY_TYPE=SHARED")
    endif()
    set(sources ${program_UNPARSED_ARGUMENTS})
    if(NOT sources)
        message(FATAL_ERROR "laiue_add_program(${name}) needs sources")
    endif()

    if(ANDROID)
        add_library(${name} SHARED ${sources})
    else()
        add_executable(${name} ${sources})
    endif()
    target_link_libraries(${name} PRIVATE laiue_bootstrap laiue_common laiue::platform_support)
    if(WIN32)
        target_sources(${name} PRIVATE
            "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../src/platform/program_entry_windows.c")
        target_link_libraries(${name} PRIVATE kernel32 shell32)
        target_link_options(${name} PRIVATE /ENTRY:LaiueProgramEntry)
        if(program_WINDOWED)
            target_link_options(${name} PRIVATE /SUBSYSTEM:WINDOWS)
        else()
            target_link_options(${name} PRIVATE /SUBSYSTEM:CONSOLE)
        endif()
    endif()

    set(modules)
    foreach(module IN LISTS program_MODULES)
        if(TARGET laiue_${module})
            list(APPEND modules laiue_${module})
        endif()
    endforeach()
    if(modules)
        add_dependencies(${name} ${modules})
    endif()

    if(ANDROID AND program_ANDROID_PACKAGE)
        if(NOT TARGET laiue_window)
            message(STATUS "${name}: no APK, the build has no window module to start it")
            return()
        endif()
        if(NOT program_ANDROID_LABEL)
            set(program_ANDROID_LABEL "${name}")
        endif()
        set(orientation)
        if(program_ANDROID_ORIENTATION)
            set(orientation ORIENTATION "${program_ANDROID_ORIENTATION}")
        endif()
        list(REMOVE_ITEM modules laiue_window)
        laiue_add_android_apk(${name}_apk
            MAIN_TARGET ${name}
            PACKAGE "${program_ANDROID_PACKAGE}"
            LABEL "${program_ANDROID_LABEL}"
            ${orientation}
            MODULES laiue_window ${modules})
    endif()
endfunction()
