function(laiue_configure_test target_name windows_entry)
    target_link_libraries(${target_name} PRIVATE
        laiue_common
        laiue::platform_support)
    if(WIN32)
        target_link_libraries(${target_name} PRIVATE kernel32)
        target_link_options(${target_name} PRIVATE
            "/ENTRY:${windows_entry}"
            /SUBSYSTEM:CONSOLE)
    endif()
endfunction()

if(UNIX)
    foreach(runtime_target IN LISTS LAIUE_RUNTIME_TARGETS)
        add_test(NAME "laiue.abi.${runtime_target}.exports"
            COMMAND "${CMAKE_COMMAND}"
                "-DNM_TOOL=${CMAKE_NM}"
                "-DLIBRARY_FILE=$<TARGET_FILE:${runtime_target}>"
                -P "${PROJECT_SOURCE_DIR}/tools/check_elf_exports.cmake")
    endforeach()
endif()

add_executable(laiue_infinite_coord_test infinite_coord_test.c)
target_link_libraries(laiue_infinite_coord_test PRIVATE laiue_world)
laiue_configure_test(laiue_infinite_coord_test CoordTestEntryPoint)
add_test(NAME laiue.world.infinite_coord COMMAND laiue_infinite_coord_test)

add_executable(laiue_world_provider_test world_provider_test.c)
target_link_libraries(laiue_world_provider_test PRIVATE laiue_world)
laiue_configure_test(laiue_world_provider_test WorldProviderTestEntryPoint)
add_test(NAME laiue.world.provider COMMAND laiue_world_provider_test)

add_executable(laiue_physics_test physics_test.c)
target_link_libraries(laiue_physics_test PRIVATE laiue_physics)
laiue_configure_test(laiue_physics_test PhysicsTestEntryPoint)
add_test(NAME laiue.physics.voxel_body COMMAND laiue_physics_test)

add_executable(laiue_physics_determinism_test physics_determinism_test.c)
target_link_libraries(laiue_physics_determinism_test PRIVATE
    laiue_physics
    laiue_world)
laiue_configure_test(
    laiue_physics_determinism_test PhysicsDeterminismTestEntryPoint)
add_test(NAME laiue.physics.determinism
    COMMAND laiue_physics_determinism_test)

add_executable(laiue_content_format_test content_format_test.c)
target_link_libraries(laiue_content_format_test PRIVATE laiue_content)
laiue_configure_test(laiue_content_format_test ContentFormatTestEntryPoint)
add_test(NAME laiue.content.format COMMAND laiue_content_format_test)

add_executable(laiue_content_catalog_test content_catalog_test.c)
target_link_libraries(laiue_content_catalog_test PRIVATE laiue_content)
laiue_configure_test(
    laiue_content_catalog_test ContentCatalogTestEntryPoint)
add_test(NAME laiue.content.catalog COMMAND laiue_content_catalog_test)

add_executable(laiue_mod_manifest_test mod_manifest_test.c)
target_link_libraries(laiue_mod_manifest_test PRIVATE laiue_mod)
laiue_configure_test(laiue_mod_manifest_test ModManifestTestEntry)
add_test(NAME laiue.mod.manifest COMMAND laiue_mod_manifest_test)

add_library(laiue_mod_test_extension SHARED
    mod_test_extension.c
    mod_test_service.h)
target_include_directories(laiue_mod_test_extension PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(laiue_mod_test_extension PRIVATE laiue_common)
if(WIN32)
    target_link_options(laiue_mod_test_extension PRIVATE /NOENTRY)
else()
    set_target_properties(laiue_mod_test_extension PROPERTIES
        C_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN YES)
endif()

add_executable(laiue_mod_host_test
    mod_host_test.c
    mod_test_service.h)
target_link_libraries(laiue_mod_host_test PRIVATE laiue_mod)
laiue_configure_test(laiue_mod_host_test ModHostTestEntry)
add_dependencies(laiue_mod_host_test laiue_mod_test_extension)
add_test(NAME laiue.mod.host COMMAND laiue_mod_host_test)

if(TARGET laiue_scene)
    add_executable(laiue_voxel_raycast_test voxel_raycast_test.c)
    target_link_libraries(laiue_voxel_raycast_test PRIVATE
        laiue_scene
        laiue_world)
    laiue_configure_test(
        laiue_voxel_raycast_test VoxelRaycastTestEntryPoint)
    add_test(NAME laiue.scene.voxel_raycast
        COMMAND laiue_voxel_raycast_test)
endif()
if(TARGET laiue_render)
    add_executable(laiue_render_pack_api_test render_pack_api_test.c)
    target_link_libraries(laiue_render_pack_api_test PRIVATE
        laiue_content
        laiue_render)
    laiue_configure_test(
        laiue_render_pack_api_test RenderPackApiTestEntryPoint)
    add_test(NAME laiue.render.packs COMMAND laiue_render_pack_api_test)
endif()
if(WIN32 AND TARGET laiue_audio)
    add_executable(laiue_audio_api_test audio_api_test.c)
    target_link_libraries(laiue_audio_api_test PRIVATE
        laiue_common
        laiue::platform_support
        laiue_audio
        kernel32
        winmm)
    target_link_options(laiue_audio_api_test PRIVATE
        /ENTRY:AudioApiTestEntryPoint
        /SUBSYSTEM:CONSOLE)
    add_test(NAME laiue.audio.api COMMAND laiue_audio_api_test)
    set_tests_properties(laiue.audio.api PROPERTIES SKIP_RETURN_CODE 125)
endif()
