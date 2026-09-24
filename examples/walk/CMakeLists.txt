if(NOT ANDROID OR NOT LAIUE_BUILD_ANDROID_WALK)
    add_executable(laiue_walk walk.c)
    target_link_libraries(laiue_walk PRIVATE laiue_bootstrap)
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
        target_link_libraries(laiue_walk PRIVATE
            laiue_window laiue_input laiue_render laiue_ui
            laiue_scene_math laiue_scene)
        target_compile_definitions(laiue_walk PRIVATE LAIUE_WALK_WINDOWED=1)
    endif()
else()
    target_compile_definitions(laiue_walk PRIVATE LAIUE_WALK_DYNAMIC=1)
    add_dependencies(laiue_walk laiue_character)
    if(LAIUE_BUILD_GRAPHICS AND (WIN32 OR X11_FOUND))
        add_dependencies(laiue_walk laiue_window laiue_input laiue_render laiue_ui
            laiue_scene_math laiue_scene)
        target_compile_definitions(laiue_walk PRIVATE LAIUE_WALK_WINDOWED=1)
    endif()
endif()
target_include_directories(laiue_walk PRIVATE
    "${PROJECT_SOURCE_DIR}/src")

    laiue_configure_standalone_executable(laiue_walk WalkExampleEntryPoint)
endif()

if(ANDROID AND LAIUE_BUILD_ANDROID_WALK)
    option(LAIUE_ANDROID_WALK_WITH_VOXEL
        "Register the optional sparse voxel provider in the Android walk profile" ON)
    add_subdirectory(android)
endif()
