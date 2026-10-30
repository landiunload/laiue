if(NOT ANDROID)
    message(FATAL_ERROR "examples/walk/android requires an Android toolchain")
endif()

set(_laiue_android_ndk "${CMAKE_ANDROID_NDK}")
if(_laiue_android_ndk STREQUAL "")
    set(_laiue_android_ndk "${ANDROID_NDK}")
endif()
set(_laiue_android_native_app_glue
    "${_laiue_android_ndk}/sources/android/native_app_glue/android_native_app_glue.c")
if(NOT EXISTS "${_laiue_android_native_app_glue}")
    message(FATAL_ERROR
        "Android NDK native_app_glue.c was not found: ${_laiue_android_native_app_glue}")
endif()

add_library(laiue_walk_android SHARED
    android_walk.c
    ../walk_terrain.h
    "${_laiue_android_native_app_glue}")
target_compile_definitions(laiue_walk_android PRIVATE
    LAIUE_STATIC=1
    LAIUE_ANDROID_WALK=1)
target_include_directories(laiue_walk_android PRIVATE
    "${PROJECT_SOURCE_DIR}/src"
    "${PROJECT_SOURCE_DIR}/src/core"
    "${PROJECT_SOURCE_DIR}/src/assets"
    "${PROJECT_SOURCE_DIR}/src/jobs"
    "${PROJECT_SOURCE_DIR}/src/simulation"
    "${PROJECT_SOURCE_DIR}/src/graphics"
    "${PROJECT_SOURCE_DIR}/src/modding"
    "${CMAKE_CURRENT_SOURCE_DIR}/.."
    "${_laiue_android_ndk}/sources/android/native_app_glue")
target_link_libraries(laiue_walk_android PRIVATE
    laiue_bootstrap
    laiue_character
    laiue_numeric
    laiue_physics
    laiue_walk_common
    laiue_render
    laiue_mesher
    laiue_scene_math
    laiue_scene
    laiue_math_support
    laiue_media_support
    android
    log
    "${Vulkan_LIBRARY}")
if(LAIUE_ANDROID_WALK_WITH_VOXEL)
    # The static registry below explicitly registers the complete voxel
    # dependency chain.  Keep all three archives in the final NativeActivity
    # image; relying on the voxel archive alone leaves the numeric/world
    # entrypoints unresolved under --no-undefined.
    target_link_libraries(laiue_walk_android PRIVATE
        laiue_world
        laiue_voxel)
    target_compile_definitions(laiue_walk_android PRIVATE LAIUE_ANDROID_WALK_WITH_VOXEL=1)
endif()
target_link_options(laiue_walk_android PRIVATE
    "-uANativeActivity_onCreate")
set_target_properties(laiue_walk_android PROPERTIES
    OUTPUT_NAME "laiue_walk"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/android/$<CONFIG>"
    POSITION_INDEPENDENT_CODE ON)

# Packaging is shared by every laiue program: laiue_add_android_apk writes
# the manifest, aligns and signs the APK. The .so itself still fits another
# application's package, since the NativeActivity ABI does not depend on it.
laiue_add_android_apk(laiue_walk_android_apk
    MAIN_TARGET laiue_walk_android
    PACKAGE dev.laiue.walk
    LABEL "LAIUE Walk"
    ASSETS "${CMAKE_CURRENT_SOURCE_DIR}/assets"
    ORIENTATION fullSensor)
