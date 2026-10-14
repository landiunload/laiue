if(NOT ANDROID OR NOT LAIUE_BUILD_ANDROID_WALK)
    add_executable(laiue_walk walk.c walk_terrain.h)
    target_link_libraries(laiue_walk PRIVATE laiue_bootstrap laiue_walk_common)
    target_compile_definitions(laiue_walk PRIVATE LAIUE_STATIC=1)
    target_include_directories(laiue_walk PRIVATE
        "${PROJECT_SOURCE_DIR}/src"
        "${PROJECT_SOURCE_DIR}/src/assets"
        "${PROJECT_SOURCE_DIR}/src/simulation")

    set(walk_windowed OFF)
    if(LAIUE_BUILD_GRAPHICS AND (WIN32 OR X11_FOUND))
        set(walk_windowed ON)
    endif()
    set(walk_static OFF)
    if(LAIUE_MODULE_LIBRARY_TYPE_RESOLVED STREQUAL "STATIC")
        set(walk_static ON)
    endif()

    # A dynamic desktop walk loads the selected backend provider, not the
    # compatibility bundle that links every available backend. The bundle
    # stays the fallback when the standalone providers are disabled.
    set(walk_graphics laiue_render)
    if(walk_windowed AND NOT walk_static)
        foreach(backend IN ITEMS D3D12 VULKAN)
            string(TOLOWER "${backend}" backend_lower)
            if(LAIUE_RENDER_BACKEND_RESOLVED STREQUAL backend AND
               TARGET laiue_graphics_${backend_lower})
                set(walk_graphics laiue_graphics_${backend_lower})
                target_compile_definitions(laiue_walk PRIVATE
                    LAIUE_WALK_EXPLICIT_GRAPHICS_PROVIDER=1
                    LAIUE_WALK_GRAPHICS_PROVIDER_${backend}=1)
            endif()
        endforeach()
    endif()

    set(walk_modules laiue_character)
    if(walk_static)
        option(LAIUE_WALK_STATIC_WITH_VOXEL
            "Register the optional voxel provider in the static walk profile" ON)
        if(LAIUE_WALK_STATIC_WITH_VOXEL)
            list(APPEND walk_modules laiue_voxel)
            target_compile_definitions(laiue_walk PRIVATE LAIUE_WALK_STATIC_WITH_VOXEL=1)
        endif()
    endif()
    if(walk_windowed)
        list(APPEND walk_modules laiue_mesher laiue_physics)
        if(walk_static AND NOT LAIUE_WALK_STATIC_WITH_VOXEL)
            list(APPEND walk_modules laiue_numeric)
        endif()
        list(APPEND walk_modules laiue_window laiue_input ${walk_graphics} laiue_ui
            laiue_scene_math laiue_scene)
        target_compile_definitions(laiue_walk PRIVATE LAIUE_WALK_WINDOWED=1)
    endif()
    # Static profiles link every module; dynamic ones only build them first.
    if(walk_static)
        target_link_libraries(laiue_walk PRIVATE ${walk_modules})
    else()
        target_compile_definitions(laiue_walk PRIVATE LAIUE_WALK_DYNAMIC=1)
        add_dependencies(laiue_walk ${walk_modules})
    endif()

    if(WIN32 AND LAIUE_BUILD_GRAPHICS)
        laiue_configure_standalone_executable(laiue_walk WalkExampleEntryPoint WINDOWED)
    else()
        laiue_configure_standalone_executable(laiue_walk WalkExampleEntryPoint)
    endif()
    if(walk_windowed)
        set(walk_textures "${CMAKE_CURRENT_SOURCE_DIR}/android/assets/textures")
        set(walk_output_textures "$<TARGET_FILE_DIR:laiue_walk>/assets/textures")
        add_custom_command(TARGET laiue_walk POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${walk_output_textures}"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${walk_textures}/grass.png" "${walk_textures}/dirt.png"
                "${walk_textures}/stone.png" "${walk_output_textures}"
            VERBATIM)
    endif()
endif()

if(ANDROID AND LAIUE_BUILD_ANDROID_WALK)
    option(LAIUE_ANDROID_WALK_WITH_VOXEL
        "Register the optional sparse voxel provider in the Android walk profile" ON)
    add_subdirectory(android)
endif()
