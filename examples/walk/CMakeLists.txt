add_library(laiue_walk_common STATIC
    walk_humanoid.c walk_humanoid.h
    walk_physics.c walk_physics.h
    walk_visuals.c walk_visuals.h
    walk_world.c walk_world.h)
target_include_directories(laiue_walk_common PRIVATE
    "${PROJECT_SOURCE_DIR}/src"
    "${PROJECT_SOURCE_DIR}/src/assets"
    "${PROJECT_SOURCE_DIR}/src/simulation")
# Physics is reached only through the laiue.physics service table (see
# walk_physics.h), so the shared walk executable has no load-time import of
# the physics library and keeps running when that artifact is removed.
target_link_libraries(laiue_walk_common PRIVATE
    laiue::build_options
    laiue::math_support
    laiue_media_support)
set_target_properties(laiue_walk_common PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(ANDROID OR LAIUE_MODULE_LIBRARY_TYPE_RESOLVED STREQUAL "STATIC")
    target_compile_definitions(laiue_walk_common PRIVATE LAIUE_STATIC=1)
endif()

if(NOT ANDROID OR NOT LAIUE_BUILD_ANDROID_WALK)
    add_executable(laiue_walk walk.c walk_terrain.h)
    target_link_libraries(laiue_walk PRIVATE laiue_bootstrap laiue_walk_common)
    target_compile_definitions(laiue_walk PRIVATE LAIUE_STATIC=1)
if(LAIUE_MODULE_LIBRARY_TYPE_RESOLVED STREQUAL "STATIC")
    option(LAIUE_WALK_STATIC_WITH_VOXEL
        "Register the optional voxel provider in the static walk profile" ON)
    target_link_libraries(laiue_walk PRIVATE laiue_character)
    if(LAIUE_WALK_STATIC_WITH_VOXEL)
        target_link_libraries(laiue_walk PRIVATE laiue_voxel)
        target_compile_definitions(laiue_walk PRIVATE LAIUE_WALK_STATIC_WITH_VOXEL=1)
    endif()
    if(LAIUE_BUILD_GRAPHICS AND (WIN32 OR X11_FOUND))
        target_link_libraries(laiue_walk PRIVATE laiue_mesher)
        target_link_libraries(laiue_walk PRIVATE laiue_physics)
        if(NOT LAIUE_WALK_STATIC_WITH_VOXEL)
            target_link_libraries(laiue_walk PRIVATE laiue_numeric)
        endif()
        # Desktop walk must load the selected backend provider, not the
        # compatibility bundle that links every available backend.  Keep the
        # bundle as a deliberate fallback for profiles that disable the
        # standalone provider targets.
        set(_laiue_walk_graphics_target laiue_render)
        if(LAIUE_MODULE_LIBRARY_TYPE_RESOLVED STREQUAL "SHARED")
            if(LAIUE_RENDER_BACKEND_RESOLVED STREQUAL "D3D12" AND
               TARGET laiue_graphics_d3d12)
                set(_laiue_walk_graphics_target laiue_graphics_d3d12)
                target_compile_definitions(laiue_walk PRIVATE
                    LAIUE_WALK_EXPLICIT_GRAPHICS_PROVIDER=1
                    LAIUE_WALK_GRAPHICS_PROVIDER_D3D12=1)
            elseif(LAIUE_RENDER_BACKEND_RESOLVED STREQUAL "VULKAN" AND
                   TARGET laiue_graphics_vulkan)
                set(_laiue_walk_graphics_target laiue_graphics_vulkan)
                target_compile_definitions(laiue_walk PRIVATE
                    LAIUE_WALK_EXPLICIT_GRAPHICS_PROVIDER=1
                    LAIUE_WALK_GRAPHICS_PROVIDER_VULKAN=1)
            endif()
        endif()
        target_link_libraries(laiue_walk PRIVATE
            laiue_window laiue_input ${_laiue_walk_graphics_target} laiue_ui
            laiue_scene_math laiue_scene)
        add_dependencies(laiue_walk ${_laiue_walk_graphics_target} laiue_mesher)
        target_compile_definitions(laiue_walk PRIVATE LAIUE_WALK_WINDOWED=1)
    endif()
else()
    target_compile_definitions(laiue_walk PRIVATE LAIUE_WALK_DYNAMIC=1)
    add_dependencies(laiue_walk laiue_character)
    if(LAIUE_BUILD_GRAPHICS AND (WIN32 OR X11_FOUND))
        add_dependencies(laiue_walk laiue_mesher)
        add_dependencies(laiue_walk laiue_physics)
        set(_laiue_walk_graphics_target laiue_render)
        if(LAIUE_RENDER_BACKEND_RESOLVED STREQUAL "D3D12" AND
           TARGET laiue_graphics_d3d12)
            set(_laiue_walk_graphics_target laiue_graphics_d3d12)
            target_compile_definitions(laiue_walk PRIVATE
                LAIUE_WALK_EXPLICIT_GRAPHICS_PROVIDER=1
                LAIUE_WALK_GRAPHICS_PROVIDER_D3D12=1)
        elseif(LAIUE_RENDER_BACKEND_RESOLVED STREQUAL "VULKAN" AND
               TARGET laiue_graphics_vulkan)
            set(_laiue_walk_graphics_target laiue_graphics_vulkan)
            target_compile_definitions(laiue_walk PRIVATE
                LAIUE_WALK_EXPLICIT_GRAPHICS_PROVIDER=1
                LAIUE_WALK_GRAPHICS_PROVIDER_VULKAN=1)
        endif()
        add_dependencies(laiue_walk laiue_window laiue_input laiue_physics
            ${_laiue_walk_graphics_target}
            laiue_ui laiue_scene_math laiue_scene)
        target_compile_definitions(laiue_walk PRIVATE LAIUE_WALK_WINDOWED=1)
    endif()
endif()
    target_include_directories(laiue_walk PRIVATE
        "${PROJECT_SOURCE_DIR}/src"
        "${PROJECT_SOURCE_DIR}/src/assets"
        "${PROJECT_SOURCE_DIR}/src/simulation")

    if(WIN32 AND LAIUE_BUILD_GRAPHICS)
        laiue_configure_standalone_executable(
            laiue_walk WalkExampleEntryPoint WINDOWED)
    else()
        laiue_configure_standalone_executable(laiue_walk WalkExampleEntryPoint)
    endif()
    if(LAIUE_BUILD_GRAPHICS AND (WIN32 OR X11_FOUND))
        set(_laiue_walk_desktop_assets
            "${CMAKE_CURRENT_SOURCE_DIR}/android/assets/textures")
        add_custom_command(TARGET laiue_walk POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E make_directory
                "$<TARGET_FILE_DIR:laiue_walk>/assets/textures"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${_laiue_walk_desktop_assets}/grass.png"
                "$<TARGET_FILE_DIR:laiue_walk>/assets/textures/grass.png"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${_laiue_walk_desktop_assets}/dirt.png"
                "$<TARGET_FILE_DIR:laiue_walk>/assets/textures/dirt.png"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${_laiue_walk_desktop_assets}/stone.png"
                "$<TARGET_FILE_DIR:laiue_walk>/assets/textures/stone.png"
            VERBATIM)
    endif()
endif()

if(ANDROID AND LAIUE_BUILD_ANDROID_WALK)
    option(LAIUE_ANDROID_WALK_WITH_VOXEL
        "Register the optional sparse voxel provider in the Android walk profile" ON)
    add_subdirectory(android)
endif()
