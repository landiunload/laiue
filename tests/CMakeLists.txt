option(LAIUE_REQUIRE_VULKAN_TEST_DRIVER
    "Fail Vulkan offscreen tests instead of skipping when no driver is available" OFF)
if(LAIUE_REQUIRE_VULKAN_TEST_DRIVER AND
   NOT (TARGET laiue_render AND LAIUE_RENDER_HAS_VULKAN_TARGET))
    message(FATAL_ERROR "LAIUE_REQUIRE_VULKAN_TEST_DRIVER requires the Vulkan render target")
endif()

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

add_executable(laiue_platform_file_test platform_file_test.c)
laiue_configure_standalone_executable(laiue_platform_file_test PlatformFileTestEntryPoint)
add_test(NAME laiue.platform.file COMMAND laiue_platform_file_test)
set_tests_properties(laiue.platform.file PROPERTIES TIMEOUT 20)

add_executable(laiue_platform_threading_test platform_threading_test.c)
laiue_configure_standalone_executable(
    laiue_platform_threading_test PlatformThreadingTestEntryPoint)
add_test(NAME laiue.platform.threading
    COMMAND laiue_platform_threading_test)

add_executable(laiue_task_pool_test task_pool_test.c)
target_link_libraries(laiue_task_pool_test PRIVATE laiue::task laiue::platform_support)
laiue_configure_standalone_executable(laiue_task_pool_test TaskPoolTestEntryPoint)
add_test(NAME laiue.task.pool COMMAND laiue_task_pool_test)
set_tests_properties(laiue.task.pool PROPERTIES TIMEOUT 20)

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

add_executable(laiue_rigid_solver_test rigid_solver_test.c)
target_link_libraries(laiue_rigid_solver_test PRIVATE laiue_physics laiue_numeric)
laiue_configure_standalone_executable(laiue_rigid_solver_test RigidSolverTestEntryPoint)
add_test(NAME laiue.physics.rigid_solver COMMAND laiue_rigid_solver_test)

add_executable(laiue_rigid_cache_test rigid_cache_test.c)
target_link_libraries(laiue_rigid_cache_test PRIVATE laiue_physics laiue_numeric)
laiue_configure_standalone_executable(laiue_rigid_cache_test RigidCacheTestEntryPoint)
add_test(NAME laiue.physics.rigid_cache COMMAND laiue_rigid_cache_test)

add_executable(laiue_rigid_broadphase_test rigid_broadphase_test.c)
target_link_libraries(laiue_rigid_broadphase_test PRIVATE laiue_physics laiue_numeric)
laiue_configure_standalone_executable(laiue_rigid_broadphase_test RigidBroadphaseTestEntryPoint)
add_test(NAME laiue.physics.rigid_broadphase COMMAND laiue_rigid_broadphase_test)

add_executable(laiue_rigid_broadphase_scale_test rigid_broadphase_scale_test.c)
target_link_libraries(laiue_rigid_broadphase_scale_test PRIVATE laiue_physics laiue_numeric)
laiue_configure_standalone_executable(
    laiue_rigid_broadphase_scale_test RigidBroadphaseScaleTestEntryPoint)
add_test(NAME laiue.physics.rigid_broadphase_scale
    COMMAND laiue_rigid_broadphase_scale_test)
set_tests_properties(laiue.physics.rigid_broadphase_scale PROPERTIES TIMEOUT 60)

add_executable(laiue_rigid_parallel_test rigid_parallel_test.c)
target_link_libraries(laiue_rigid_parallel_test PRIVATE laiue::physics laiue::numeric laiue::task)
laiue_configure_standalone_executable(laiue_rigid_parallel_test RigidParallelTestEntryPoint)
add_test(NAME laiue.physics.rigid_parallel COMMAND laiue_rigid_parallel_test)
set_tests_properties(laiue.physics.rigid_parallel PROPERTIES TIMEOUT 60)

# Составные вращающиеся тела: массовые свойства, обратный тензор 3x3,
# вогнутая L-форма, взаимный импульс, warm-start cache, детерминированный
# replay, эквивалентность executor-ов и rebasing.
add_executable(laiue_rigid_compound_test rigid_compound_test.c)
target_link_libraries(laiue_rigid_compound_test PRIVATE
    laiue::physics
    laiue::numeric
    laiue::task)
laiue_configure_standalone_executable(
    laiue_rigid_compound_test RigidCompoundTestEntryPoint)
add_test(NAME laiue.physics.rigid_compound COMMAND laiue_rigid_compound_test)
set_tests_properties(laiue.physics.rigid_compound PROPERTIES TIMEOUT 120)

add_executable(laiue_compound_shape_test compound_shape_test.c)
target_link_libraries(laiue_compound_shape_test PRIVATE laiue::physics)
laiue_configure_standalone_executable(laiue_compound_shape_test CompoundShapeTestEntryPoint)
add_test(NAME laiue.physics.compound_shape COMMAND laiue_compound_shape_test)
set_tests_properties(laiue.physics.compound_shape PROPERTIES TIMEOUT 30)

# Test the private spatial helper directly; it is not an exported SDK ABI.
add_executable(laiue_compound_bvh_test compound_bvh_test.c ../src/physics/compound_bvh.c)
target_link_libraries(laiue_compound_bvh_test PRIVATE laiue::physics)
if(LAIUE_PLATFORM_EXTERNAL)
    target_link_libraries(laiue_compound_bvh_test PRIVATE "${LAIUE_PRECISE_FP_TARGET}")
elseif(LAIUE_PLATFORM_WINDOWS)
    target_compile_options(laiue_compound_bvh_test PRIVATE
        "$<$<C_COMPILER_ID:MSVC>:/fp:strict>"
        "$<$<C_COMPILER_ID:Clang>:/clang:-ffp-model=strict>")
else()
    target_compile_options(laiue_compound_bvh_test PRIVATE -fno-fast-math -ffp-contract=off)
endif()
laiue_configure_standalone_executable(laiue_compound_bvh_test CompoundBvhTestEntryPoint)
add_test(NAME laiue.physics.compound_bvh COMMAND laiue_compound_bvh_test)
set_tests_properties(laiue.physics.compound_bvh PROPERTIES TIMEOUT 30)

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

if(TARGET laiue_mesher)
    add_executable(laiue_chunk_mesher_test chunk_mesher_test.c)
    target_link_libraries(laiue_chunk_mesher_test PRIVATE laiue::mesher laiue::world)
    laiue_configure_standalone_executable(
        laiue_chunk_mesher_test ChunkMesherTestEntryPoint)
    add_test(NAME laiue.mesh.chunk_mesher COMMAND laiue_chunk_mesher_test)
    set_tests_properties(laiue.mesh.chunk_mesher PROPERTIES TIMEOUT 120)
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

    # Стресс таблицы чанков: случайные переходы и блуждание по кольцу с
    # независимым эталоном множества ключей и проверкой списка отрисовки.
    add_executable(laiue_chunk_streaming_stress_test
        chunk_streaming_stress_test.c)
    target_link_libraries(laiue_chunk_streaming_stress_test PRIVATE
        laiue_scene
        laiue_world)
    laiue_configure_standalone_executable(
        laiue_chunk_streaming_stress_test ChunkStreamingStressTestEntryPoint)
    add_test(NAME laiue.scene.chunk_streaming_stress
        COMMAND laiue_chunk_streaming_stress_test)
    set_tests_properties(laiue.scene.chunk_streaming_stress PROPERTIES
        TIMEOUT 120)
endif()
if(TARGET laiue_render)
    add_executable(laiue_render_pack_api_test render_pack_api_test.c)
    target_link_libraries(laiue_render_pack_api_test PRIVATE
        laiue_content
        laiue_render)
    laiue_configure_standalone_executable(
        laiue_render_pack_api_test RenderPackApiTestEntryPoint)
    add_test(NAME laiue.render.packs COMMAND laiue_render_pack_api_test)

    # Двухпроходная сборка пака проверяется напрямую, без GPU и без новых
    # экспортов: цель компилирует внутренний texture_build.c в тестовый
    # исполняемый файл. Сквозной путь по цвету пикселя отдельно проверяет
    # laiue.render.offscreen_frame, но требует настоящего драйвера; этот
    # тест детерминирован и идёт на любой машине.
    add_executable(laiue_texture_pack_build_test
        texture_pack_build_test.c
        texc_fixtures.h
        "${PROJECT_SOURCE_DIR}/src/render/texture_build.c")
    target_include_directories(laiue_texture_pack_build_test PRIVATE
        "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(laiue_texture_pack_build_test PRIVATE
        laiue_content
        laiue::media_support)
    # The test source marks its large scenario functions noinline so Release
    # LTO cannot flatten every TexturePackData frame into the entry point and
    # reintroduce a __chkstk-only dependency in this no-CRT executable.
    laiue_configure_standalone_executable(
        laiue_texture_pack_build_test TexturePackBuildTestEntryPoint)
    add_test(NAME laiue.render.texture_build
        COMMAND laiue_texture_pack_build_test)
endif()
if(TARGET laiue_render AND LAIUE_RENDER_HAS_VULKAN_TARGET)
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

    # Адаптивное Vulkan instance-кольцо: четыре вызова по 200000 инстансов
    # превышают прежние 16 МиБ на кадр. Проверяются оба кадровых слота,
    # rollover чанков и resize; без Vulkan-драйвера тест сообщает SKIP.
    add_executable(laiue_renderer_vulkan_instance_ring_test
        renderer_vulkan_instance_ring_test.c)
    target_link_libraries(laiue_renderer_vulkan_instance_ring_test PRIVATE
        laiue_render)
    laiue_configure_standalone_executable(
        laiue_renderer_vulkan_instance_ring_test RendererVulkanInstanceRingTestEntryPoint)
    add_test(NAME laiue.render.vulkan_instance_ring
        COMMAND laiue_renderer_vulkan_instance_ring_test)
    set_tests_properties(laiue.render.vulkan_instance_ring PROPERTIES
        TIMEOUT 180)
    set_tests_properties(laiue.render.offscreen_frame
        laiue.render.vulkan_instance_ring PROPERTIES
        FAIL_REGULAR_EXPRESSION "Validation Error")
    if(NOT LAIUE_REQUIRE_VULKAN_TEST_DRIVER)
        set_tests_properties(laiue.render.offscreen_frame
            laiue.render.vulkan_instance_ring PROPERTIES SKIP_RETURN_CODE 125)
    endif()

    # Жизненный цикл окна/swapchain Vulkan: повторное создание и
    # уничтожение, resize/minimize/restore, смена vsync, переключение
    # D3D12<->Vulkan и несколько кадровых слотов. Окна скрытые; без
    # Vulkan-драйвера или рабочей поверхности тест сообщает о пропуске,
    # а не о ложном успехе.
    add_executable(laiue_renderer_vulkan_lifecycle_test
        renderer_vulkan_lifecycle_test.c)
    target_link_libraries(laiue_renderer_vulkan_lifecycle_test PRIVATE
        laiue_render)
    laiue_configure_standalone_executable(
        laiue_renderer_vulkan_lifecycle_test RendererVulkanLifecycleTestEntryPoint)
    add_test(NAME laiue.render.vulkan_lifecycle
        COMMAND laiue_renderer_vulkan_lifecycle_test)
    set_tests_properties(laiue.render.vulkan_lifecycle PROPERTIES
        SKIP_RETURN_CODE 125
        FAIL_REGULAR_EXPRESSION "Validation Error"
        TIMEOUT 180)
endif()
if(TARGET laiue_render AND LAIUE_RENDER_HAS_D3D12
   AND LAIUE_RENDER_HAS_VULKAN_TARGET)
    # Оба бэкенда живут в одном процессе и чередуются на кадрах: это бьёт
    # по одноэлементному кэшу быстрого пути диспетчера. D3D12 нужен
    # настоящий HWND (скрытое окно), Vulkan рисует offscreen. Без
    # адаптера D3D12 тест сообщает о пропуске кодом 125.
    add_executable(laiue_render_dual_backend_test
        render_dual_backend_test.c)
    target_link_libraries(laiue_render_dual_backend_test PRIVATE
        laiue_render)
    laiue_configure_standalone_executable(
        laiue_render_dual_backend_test RenderDualBackendTestEntryPoint)
    add_test(NAME laiue.render.dual_backend
        COMMAND laiue_render_dual_backend_test)
    set_tests_properties(laiue.render.dual_backend PROPERTIES
        SKIP_RETURN_CODE 125
        FAIL_REGULAR_EXPRESSION "Validation Error"
        TIMEOUT 60)
endif()
if(TARGET laiue_render AND LAIUE_RENDER_HAS_D3D12)
    # Адаптивный чанковый instance ring: вызовы сверх прежних 16 МиБ на кадр
    # не теряются, смена кадрового слота и resize не рвут пул. Нужен
    # настоящий HWND; без адаптера D3D12 тест сообщает о пропуске кодом 125.
    add_executable(laiue_renderer_instance_ring_test
        renderer_instance_ring_test.c)
    target_link_libraries(laiue_renderer_instance_ring_test PRIVATE
        laiue_render)
    laiue_configure_standalone_executable(
        laiue_renderer_instance_ring_test RendererInstanceRingTestEntryPoint)
    add_test(NAME laiue.render.instance_ring
        COMMAND laiue_renderer_instance_ring_test)
    set_tests_properties(laiue.render.instance_ring PROPERTIES
        SKIP_RETURN_CODE 125
        TIMEOUT 60)
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

if(TARGET laiue_media_support)
    # Быстрые пути ресемплера сверяются побайтово с локальной копией
    # прежнего общего пути на десятках тысяч случайных изображений.
    add_executable(laiue_image_resample_test image_resample_test.c)
    target_link_libraries(laiue_image_resample_test PRIVATE laiue::media_support)
    laiue_configure_standalone_executable(
        laiue_image_resample_test ImageResampleTestEntryPoint)
    add_test(NAME laiue.media.resample COMMAND laiue_image_resample_test)

    # Эквивалентность декодеров PNG и JPEG после ускорения Хаффмана:
    # эталонные суммы сняты с исходной побитовой реализации.
    add_executable(laiue_image_decode_test image_decode_test.c texc_fixtures.h)
    target_link_libraries(laiue_image_decode_test PRIVATE laiue::media_support)
    laiue_configure_standalone_executable(laiue_image_decode_test ImageDecodeTestEntryPoint)
    add_test(NAME laiue.media.image_decode COMMAND laiue_image_decode_test)
endif()
