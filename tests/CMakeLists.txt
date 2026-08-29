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

add_executable(laiue_content_format_test content_format_test.c)
target_link_libraries(laiue_content_format_test PRIVATE laiue_content)
laiue_configure_test(laiue_content_format_test ContentFormatTestEntryPoint)
add_test(NAME laiue.content.format COMMAND laiue_content_format_test)

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
