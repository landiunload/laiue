if(LAIUE_PLATFORM_EXTERNAL)
    # A closed parent superbuild owns the adapter and SDK libraries. The
    # public engine only consumes the stable Platform* contract.
    add_library(laiue_platform_support INTERFACE)
    target_link_libraries(laiue_platform_support INTERFACE
        "${LAIUE_PLATFORM_ADAPTER_TARGET}")

    # Source-based adapters participate in the same warnings, architecture
    # checks and Release optimization profile as the engine modules. Imported
    # proprietary SDK targets remain untouched and keep their vendor flags.
    if(LAIUE_ADAPTER_USE_ENGINE_BUILD_OPTIONS)
        get_target_property(_laiue_adapter_aliased_target
            "${LAIUE_PLATFORM_ADAPTER_TARGET}" ALIASED_TARGET)
        if(_laiue_adapter_aliased_target)
            set(_laiue_adapter_inspected_target
                "${_laiue_adapter_aliased_target}")
        else()
            set(_laiue_adapter_inspected_target
                "${LAIUE_PLATFORM_ADAPTER_TARGET}")
        endif()
        get_target_property(_laiue_adapter_imported
            "${_laiue_adapter_inspected_target}" IMPORTED)
        if(_laiue_adapter_imported)
            message(FATAL_ERROR
                "LAIUE_ADAPTER_USE_ENGINE_BUILD_OPTIONS cannot mutate an imported adapter")
        endif()
        get_target_property(_laiue_adapter_type
            "${_laiue_adapter_inspected_target}" TYPE)
        if(_laiue_adapter_type STREQUAL "INTERFACE_LIBRARY")
            target_link_libraries("${_laiue_adapter_inspected_target}"
                INTERFACE laiue::build_options)
        else()
            target_link_libraries("${_laiue_adapter_inspected_target}"
                PRIVATE laiue::build_options)
        endif()
    endif()
else()
    if(LAIUE_PLATFORM_WINDOWS)
        set(laiue_platform_system_sources system_windows.c)
    else()
        set(laiue_platform_system_sources
            system_posix.c
            sha256.c
            sha256.h)
        find_package(Threads REQUIRED)
        if(APPLE)
            find_library(LAIUE_SECURITY_FRAMEWORK Security REQUIRED)
        endif()
    endif()

    # Headless OS boundary. It is static so internal Platform* symbols do not
    # become part of the public module ABI.
    add_library(laiue_platform_support STATIC
        ${laiue_platform_system_sources}
        system.h)
    target_link_libraries(laiue_platform_support
        PRIVATE laiue::build_options)
    set_target_properties(laiue_platform_support PROPERTIES
        POSITION_INDEPENDENT_CODE ON)
    if(LAIUE_PLATFORM_POSIX)
        set_target_properties(laiue_platform_support PROPERTIES
            C_VISIBILITY_PRESET hidden
            VISIBILITY_INLINES_HIDDEN YES)
    endif()

    if(LAIUE_PLATFORM_WINDOWS)
        target_link_libraries(laiue_platform_support
            PUBLIC kernel32 bcrypt)
    elseif(APPLE)
        target_link_libraries(laiue_platform_support
            PUBLIC Threads::Threads ${LAIUE_SECURITY_FRAMEWORK} ${CMAKE_DL_LIBS})
    else()
        target_link_libraries(laiue_platform_support
            PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
    endif()
endif()
add_library(laiue::platform_support ALIAS laiue_platform_support)

# The window boundary is part of the optional Windows graphics layer.
if(LAIUE_BUILD_GRAPHICS)
    laiue_add_module(window
        RUNTIME_ROLE GRAPHICS
        SOURCES
            window.c
            window.h
            time.c
            time.h
            ../api.h
        WINDOWS_LINK
            user32
            gdi32)
endif()
