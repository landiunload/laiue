if(LAIUE_MODULE_LIBRARY_TYPE_RESOLVED STREQUAL "SHARED" AND
   CMAKE_SYSTEM_NAME STREQUAL "Linux")
    foreach(runtime_target IN LISTS LAIUE_RUNTIME_TARGETS)
        add_test(NAME "laiue.abi.${runtime_target}.exports"
            COMMAND "${CMAKE_COMMAND}"
                "-DNM_TOOL=${CMAKE_NM}"
                "-DLIBRARY_FILE=$<TARGET_FILE:${runtime_target}>"
                -P "${PROJECT_SOURCE_DIR}/tools/check_elf_exports.cmake")
    endforeach()
elseif(LAIUE_MODULE_LIBRARY_TYPE_RESOLVED STREQUAL "SHARED" AND APPLE)
    foreach(runtime_target IN LISTS LAIUE_RUNTIME_TARGETS)
        add_test(NAME "laiue.abi.${runtime_target}.exports"
            COMMAND "${CMAKE_COMMAND}"
                "-DNM_TOOL=${CMAKE_NM}"
                "-DLIBRARY_FILE=$<TARGET_FILE:${runtime_target}>"
                -P "${PROJECT_SOURCE_DIR}/tools/check_macho_exports.cmake")
    endforeach()
endif()

add_executable(laiue_platform_crypto_test platform_crypto_test.c)
laiue_configure_standalone_executable(laiue_platform_crypto_test PlatformCryptoTestEntry)
add_test(NAME laiue.platform.crypto COMMAND laiue_platform_crypto_test)

add_executable(laiue_platform_threading_test platform_threading_test.c)
laiue_configure_standalone_executable(
    laiue_platform_threading_test PlatformThreadingTestEntryPoint)
add_test(NAME laiue.platform.threading
    COMMAND laiue_platform_threading_test)

add_executable(laiue_infinite_coord_test infinite_coord_test.c)
target_link_libraries(laiue_infinite_coord_test PRIVATE laiue_numeric)
laiue_configure_standalone_executable(laiue_infinite_coord_test CoordTestEntryPoint)
add_test(NAME laiue.numeric.infinite_coord COMMAND laiue_infinite_coord_test)

add_executable(laiue_world_provider_test world_provider_test.c)
target_link_libraries(laiue_world_provider_test PRIVATE laiue_world)
laiue_configure_standalone_executable(laiue_world_provider_test WorldProviderTestEntryPoint)
add_test(NAME laiue.world.provider COMMAND laiue_world_provider_test)

add_executable(laiue_physics_test physics_test.c)
target_link_libraries(laiue_physics_test PRIVATE laiue_physics)
laiue_configure_standalone_executable(laiue_physics_test PhysicsTestEntryPoint)
add_test(NAME laiue.physics.voxel_body COMMAND laiue_physics_test)

add_executable(laiue_rigid_body_test rigid_body_test.c)
target_link_libraries(laiue_rigid_body_test PRIVATE laiue_physics)
laiue_configure_standalone_executable(laiue_rigid_body_test RigidBodyTestEntryPoint)
add_test(NAME laiue.physics.rigid_body COMMAND laiue_rigid_body_test)

add_executable(laiue_physics_determinism_test physics_determinism_test.c)
target_link_libraries(laiue_physics_determinism_test PRIVATE
    laiue_physics
    laiue_numeric
    laiue_world)
laiue_configure_standalone_executable(
    laiue_physics_determinism_test PhysicsDeterminismTestEntryPoint)
add_test(NAME laiue.physics.determinism
    COMMAND laiue_physics_determinism_test)

add_executable(laiue_content_format_test content_format_test.c)
target_link_libraries(laiue_content_format_test PRIVATE laiue_content)
laiue_configure_standalone_executable(laiue_content_format_test ContentFormatTestEntryPoint)
add_test(NAME laiue.content.format COMMAND laiue_content_format_test)

add_executable(laiue_content_catalog_test content_catalog_test.c)
target_link_libraries(laiue_content_catalog_test PRIVATE laiue_content)
laiue_configure_standalone_executable(
    laiue_content_catalog_test ContentCatalogTestEntryPoint)
add_test(NAME laiue.content.catalog COMMAND laiue_content_catalog_test)

add_executable(laiue_mod_manifest_test mod_manifest_test.c)
target_link_libraries(laiue_mod_manifest_test PRIVATE laiue_mod)
laiue_configure_standalone_executable(laiue_mod_manifest_test ModManifestTestEntry)
add_test(NAME laiue.mod.manifest COMMAND laiue_mod_manifest_test)

if(LAIUE_NATIVE_MODS_ENABLED)
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
    laiue_configure_standalone_executable(laiue_mod_host_test ModHostTestEntry)
    add_dependencies(laiue_mod_host_test laiue_mod_test_extension)
    add_test(NAME laiue.mod.host COMMAND laiue_mod_host_test)
else()
    add_executable(laiue_mod_host_disabled_test mod_host_disabled_test.c)
    target_link_libraries(laiue_mod_host_disabled_test PRIVATE laiue_mod)
    laiue_configure_standalone_executable(laiue_mod_host_disabled_test ModHostDisabledTestEntry)
    add_test(NAME laiue.mod.host_disabled COMMAND laiue_mod_host_disabled_test)
endif()

if(TARGET laiue_scene)
    add_executable(laiue_voxel_raycast_test voxel_raycast_test.c)
    target_link_libraries(laiue_voxel_raycast_test PRIVATE
        laiue_scene
        laiue_world)
    laiue_configure_standalone_executable(
        laiue_voxel_raycast_test VoxelRaycastTestEntryPoint)
    add_test(NAME laiue.scene.voxel_raycast
        COMMAND laiue_voxel_raycast_test)
endif()
if(TARGET laiue_render)
    add_executable(laiue_render_pack_api_test render_pack_api_test.c)
    target_link_libraries(laiue_render_pack_api_test PRIVATE
        laiue_content
        laiue_render)
    laiue_configure_standalone_executable(
        laiue_render_pack_api_test RenderPackApiTestEntryPoint)
    add_test(NAME laiue.render.packs COMMAND laiue_render_pack_api_test)
endif()
if(TARGET laiue_render AND LAIUE_RENDER_BACKEND_RESOLVED STREQUAL "VULKAN")
    # Кадр рисуется целиком и проверяется по пикселям. Драйвер может
    # отсутствовать на машине разработчика, поэтому тест сам сообщает об
    # этом кодом возврата, а не падает молча.
    add_executable(laiue_renderer_offscreen_test
        renderer_offscreen_test.c
        texc_fixtures.h)
    target_link_libraries(laiue_renderer_offscreen_test PRIVATE
        laiue_content
        laiue_render)
    laiue_configure_standalone_executable(
        laiue_renderer_offscreen_test RendererOffscreenTestEntryPoint)
    add_test(NAME laiue.render.offscreen_frame
        COMMAND laiue_renderer_offscreen_test)
endif()
if(TARGET laiue_audio)
    # Тест берёт offscreen-бэкенд и потому не требует звукового
    # устройства: он одинаково идёт на любой платформе и в контейнере.
    add_executable(laiue_audio_api_test audio_api_test.c)
    target_link_libraries(laiue_audio_api_test PRIVATE laiue_audio)
    laiue_configure_standalone_executable(laiue_audio_api_test AudioApiTestEntryPoint)
    add_test(NAME laiue.audio.mixer COMMAND laiue_audio_api_test)

    add_executable(laiue_audio_pack_test audio_pack_test.c mp3_fixtures.h)
    target_link_libraries(laiue_audio_pack_test PRIVATE laiue_audio laiue_content)
    laiue_configure_standalone_executable(
        laiue_audio_pack_test AudioPackTestEntryPoint)
    add_test(NAME laiue.audio.packs COMMAND laiue_audio_pack_test)

    # Системный вывод проверяется отдельно и беззвучно. Там, где
    # звукового устройства нет, тест сообщает о пропуске кодом 125.
    add_executable(laiue_audio_system_backend_test audio_system_backend_test.c)
    target_link_libraries(laiue_audio_system_backend_test PRIVATE laiue_audio)
    laiue_configure_standalone_executable(
        laiue_audio_system_backend_test AudioSystemBackendTestEntryPoint)
    add_test(NAME laiue.audio.system_backend
        COMMAND laiue_audio_system_backend_test)
    set_tests_properties(laiue.audio.system_backend PROPERTIES
        SKIP_RETURN_CODE 125
        TIMEOUT 30)
endif()

if(TARGET laiue_media_support)
    # Запись `.la` проверяется вместе с движком: круг замыкается
    # настоящим декодером, если звуковая подсистема в этом профиле
    # собрана. Без неё остаются разбор WAV и размеры контейнера — они
    # от звука не зависят.
    add_executable(laiue_soundc_test soundc_test.c)
    target_link_libraries(laiue_soundc_test PRIVATE laiue::media_support)
    if(TARGET laiue_audio)
        target_link_libraries(laiue_soundc_test PRIVATE laiue_audio)
        target_compile_definitions(laiue_soundc_test PRIVATE
            LAIUE_SOUNDC_TEST_WITH_AUDIO=1)
    endif()
    laiue_configure_standalone_executable(laiue_soundc_test SoundcTestEntryPoint)
    add_test(NAME laiue.tools.soundc COMMAND laiue_soundc_test)
endif()

if(TARGET laiue_media_support)
    add_executable(laiue_mp3_test mp3_test.c mp3_fixtures.h)
    target_link_libraries(laiue_mp3_test PRIVATE laiue::media_support)
    laiue_configure_standalone_executable(laiue_mp3_test Mp3TestEntryPoint)
    add_test(NAME laiue.media.mp3 COMMAND laiue_mp3_test)
endif()

if(TARGET laiue_media_support)
    add_executable(laiue_texc_test texc_test.c texc_fixtures.h)
    target_link_libraries(laiue_texc_test PRIVATE laiue::media_support)
    laiue_configure_standalone_executable(laiue_texc_test TexcTestEntryPoint)
    add_test(NAME laiue.tools.texc COMMAND laiue_texc_test)
endif()
