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
        set(laiue_platform_system_sources
            system_windows.c
            efficiency_windows.c
            efficiency_windows.h)
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
    # Installed with the SDK beside bootstrap, so it carries no LTO objects.
    set_target_properties(laiue_platform_support PROPERTIES
        POSITION_INDEPENDENT_CODE ON
        LAIUE_PORTABLE_ARCHIVE ON)
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

# Окно — самостоятельный platform provider, а не свойство конкретного
# graphics backend-а. Поэтому Win32-окно собирается и для D3D12, и для
# Vulkan; headless Vulkan по-прежнему можно использовать без загрузки этого
# модуля. Нативные Wayland/X11/mobile providers подключаются внешним adapter.
if(LAIUE_BUILD_GRAPHICS AND LAIUE_PLATFORM_WINDOWS)
    laiue_add_module(window
        RUNTIME_ROLE GRAPHICS
        SOURCES
            window.c
            window.h
            window_service.h
            window_module_entry.c
            time.c
            time.h
            ../api.h
        WINDOWS_LINK
            user32
            gdi32)
elseif(LAIUE_BUILD_GRAPHICS AND LAIUE_PLATFORM_POSIX AND X11_FOUND)
    # time.c — реализация на QueryPerformanceCounter и только для Win32;
    # X11-окну она не нужна, а монотонные часы POSIX есть в platform_support.
    laiue_add_module(window
        RUNTIME_ROLE GRAPHICS
        SOURCES
            window_x11.c
            window.h
            window_service.h
            window_module_entry.c
            ../api.h
        POSIX_LINK
            X11::X11)
elseif(LAIUE_BUILD_GRAPHICS AND ANDROID)
    # NativeActivity: окно и точка входа android_main. native_app_glue —
    # исходник NDK: её поток событий и есть цикл окна, поэтому она входит в
    # модуль окна, но собирается правилами NDK, а не предупреждениями движка.
    set(_laiue_android_ndk "${CMAKE_ANDROID_NDK}")
    if(_laiue_android_ndk STREQUAL "")
        set(_laiue_android_ndk "${ANDROID_NDK}")
    endif()
    set(_laiue_android_glue "${_laiue_android_ndk}/sources/android/native_app_glue")
    if(NOT EXISTS "${_laiue_android_glue}/android_native_app_glue.c")
        message(FATAL_ERROR
            "Android NDK native_app_glue was not found: ${_laiue_android_glue}")
    endif()
    add_library(laiue_android_native_app_glue OBJECT
        "${_laiue_android_glue}/android_native_app_glue.c")
    # Наружу из glue видна только ANativeActivity_onCreate: она объявлена
    # JNIEXPORT, а остальное — внутренности цикла окна.
    set_target_properties(laiue_android_native_app_glue PROPERTIES
        POSITION_INDEPENDENT_CODE ON
        C_VISIBILITY_PRESET hidden)
    laiue_add_module(window
        RUNTIME_ROLE GRAPHICS
        SOURCES
            window.h
            window_service.h
            window_module_entry.c
            ../api.h
        EXTERNAL_SOURCES
            window_android.c
        EXTERNAL_LINK
            android
            log)
    target_sources(laiue_window PRIVATE $<TARGET_OBJECTS:laiue_android_native_app_glue>)
    target_include_directories(laiue_window SYSTEM PRIVATE "${_laiue_android_glue}")
    if(LAIUE_MODULE_LIBRARY_TYPE_RESOLVED STREQUAL "STATIC")
        # Статическое окно входит в библиотеку программы. NativeActivity
        # находит её по экспортированной ANativeActivity_onCreate; без явной
        # ссылки компоновщик выбросил бы её из архива.
        target_link_options(laiue_window INTERFACE "-uANativeActivity_onCreate")
    endif()
endif()
