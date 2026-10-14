option(LAIUE_REQUIRE_VULKAN_TEST_DRIVER
    "Fail Vulkan offscreen tests instead of skipping when no driver is available" OFF)
option(LAIUE_REQUIRE_D3D12_TEST_DRIVER
    "Fail D3D12 geometry tests instead of skipping when no driver is available" OFF)
# Тесты уже считают "Validation Error" провалом, поэтому Khronos validation
# layer превращает их в проверку spec-корректности. Нужен Vulkan SDK.
option(LAIUE_VULKAN_VALIDATION
    "Run Vulkan renderer tests under VK_LAYER_KHRONOS_validation" OFF)
if(LAIUE_REQUIRE_VULKAN_TEST_DRIVER AND
   NOT (TARGET laiue_render AND LAIUE_RENDER_HAS_VULKAN_TARGET))
    message(FATAL_ERROR "LAIUE_REQUIRE_VULKAN_TEST_DRIVER requires the Vulkan render target")
endif()
if(LAIUE_REQUIRE_D3D12_TEST_DRIVER AND
   NOT (TARGET laiue_render AND LAIUE_RENDER_HAS_D3D12 AND WIN32))
    message(FATAL_ERROR "LAIUE_REQUIRE_D3D12_TEST_DRIVER requires the Windows D3D12 render target")
endif()

set(laiue_src "${PROJECT_SOURCE_DIR}/src")
set(laiue_walk_example "${PROJECT_SOURCE_DIR}/examples/walk")
if(LAIUE_MODULE_LIBRARY_TYPE_RESOLVED STREQUAL "SHARED")
    set(laiue_shared_modules ON)
else()
    set(laiue_shared_modules OFF)
endif()

# GPU tests fail on validation errors. Exit code 125 reports a missing driver
# and counts as a skip unless the driver is required.
function(laiue_set_gpu_test_properties require_driver)
    set_tests_properties(${ARGN} PROPERTIES FAIL_REGULAR_EXPRESSION "Validation Error")
    if(NOT require_driver)
        set_tests_properties(${ARGN} PROPERTIES SKIP_RETURN_CODE 125)
    endif()
endfunction()

function(laiue_add_vulkan_validation)
    if(LAIUE_VULKAN_VALIDATION)
        set(environment "VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation")
        if(DEFINED ENV{VULKAN_SDK})
            list(APPEND environment "VK_LAYER_PATH=$ENV{VULKAN_SDK}/Bin")
        endif()
        set_property(TEST ${ARGN} APPEND PROPERTY ENVIRONMENT ${environment})
    endif()
endfunction()

# Native fixtures load like mods: no CRT entry point and explicit exports only.
function(laiue_add_test_module target_name)
    add_library(${target_name} SHARED ${ARGN})
    target_include_directories(${target_name} PRIVATE
        "${PROJECT_SOURCE_DIR}/src" "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(${target_name} PRIVATE laiue_common)
    if(WIN32)
        target_link_options(${target_name} PRIVATE /NOENTRY)
    else()
        set_target_properties(${target_name} PROPERTIES
            C_VISIBILITY_PRESET hidden
            VISIBILITY_INLINES_HIDDEN YES)
    endif()
endfunction()

# Shared modules export only the public ABI and have no load-time imports of
# each other, so removing one artifact never breaks the rest.
if(laiue_shared_modules AND (CMAKE_SYSTEM_NAME STREQUAL "Linux" OR APPLE))
    if(APPLE)
        set(laiue_exports_check check_macho_exports.cmake)
    else()
        set(laiue_exports_check check_elf_exports.cmake)
    endif()
    foreach(runtime_target IN LISTS LAIUE_RUNTIME_TARGETS)
        add_test(NAME "laiue.abi.${runtime_target}.exports"
            COMMAND "${CMAKE_COMMAND}"
                "-DNM_TOOL=${CMAKE_NM}"
                "-DLIBRARY_FILE=$<TARGET_FILE:${runtime_target}>"
                -P "${PROJECT_SOURCE_DIR}/tools/${laiue_exports_check}")
        if(NOT APPLE)
            add_test(NAME "laiue.abi.${runtime_target}.imports"
                COMMAND "${CMAKE_COMMAND}"
                    "-DREADELF_TOOL=${CMAKE_READELF}"
                    "-DBINARY_FILE=$<TARGET_FILE:${runtime_target}>"
                    -P "${PROJECT_SOURCE_DIR}/tools/check_elf_imports.cmake")
        endif()
    endforeach()
    if(NOT APPLE AND TARGET laiue_walk)
        add_test(NAME laiue.example.walk_imports
            COMMAND "${CMAKE_COMMAND}"
                "-DREADELF_TOOL=${CMAKE_READELF}"
                "-DBINARY_FILE=$<TARGET_FILE:laiue_walk>"
                -P "${PROJECT_SOURCE_DIR}/tools/check_elf_imports.cmake")
    endif()
endif()

# Platform.
laiue_add_harness(laiue_platform_crypto_test PlatformCryptoTestEntry platform_crypto_test.c
    TEST laiue.platform.crypto)
laiue_add_harness(laiue_platform_file_test PlatformFileTestEntryPoint platform_file_test.c
    TEST laiue.platform.file TIMEOUT 20)
if(LAIUE_PLATFORM_POSIX OR LAIUE_PLATFORM_EXTERNAL)
    set_tests_properties(laiue.platform.file PROPERTIES
        ENVIRONMENT "LAIUE_PLATFORM_FILE_TEST_ROOT=/tmp")
endif()
laiue_add_harness(laiue_platform_threading_test PlatformThreadingTestEntryPoint
    platform_threading_test.c TEST laiue.platform.threading)
# Windows hashes through BCrypt, so the portable sha256.c is compiled in here.
set(laiue_sha256_sources sha256_test.c)
if(LAIUE_PLATFORM_WINDOWS)
    list(APPEND laiue_sha256_sources "${laiue_src}/platform/sha256.c")
endif()
laiue_add_harness(laiue_sha256_test Sha256TestEntryPoint ${laiue_sha256_sources}
    INCLUDES "${laiue_src}" TEST laiue.platform.sha256 TIMEOUT 60)
laiue_add_harness(laiue_task_pool_test TaskPoolTestEntryPoint task_pool_test.c
    LIBRARIES laiue::task laiue::platform_support TEST laiue.task.pool TIMEOUT 20)

# World and numeric.
laiue_add_harness(laiue_infinite_coord_test CoordTestEntryPoint infinite_coord_test.c
    LIBRARIES laiue_numeric TEST laiue.numeric.infinite_coord)
laiue_add_harness(laiue_world_provider_test WorldProviderTestEntryPoint world_provider_test.c
    LIBRARIES laiue_world laiue_numeric TEST laiue.world.provider)
laiue_add_harness(laiue_world_shift_test WorldShiftTestEntryPoint world_shift_test.c
    LIBRARIES laiue_world laiue_numeric TEST laiue.world.shift)
if(TARGET laiue_world AND laiue_shared_modules)
    laiue_add_harness(laiue_world_module_test WorldModuleTestEntryPoint world_module_test.c
        BOOTSTRAP DEPENDS laiue_world TEST laiue.world.module)
endif()

# Physics.
laiue_add_harness(laiue_physics_test PhysicsTestEntryPoint physics_test.c
    LIBRARIES laiue_physics laiue_numeric TEST laiue.physics.voxel_body)
laiue_add_harness(laiue_physics_determinism_test PhysicsDeterminismTestEntryPoint
    physics_determinism_test.c LIBRARIES laiue_physics laiue_numeric laiue_world
    TEST laiue.physics.determinism)
laiue_add_harness(laiue_rigid_body_test RigidBodyTestEntryPoint rigid_body_test.c
    LIBRARIES laiue_physics laiue_numeric TEST laiue.physics.rigid_body)
laiue_add_harness(laiue_rigid_velocity_add_test RigidVelocityAddTestEntryPoint
    rigid_velocity_add_test.c LIBRARIES laiue_physics laiue_numeric
    TEST laiue.physics.velocity_add)
laiue_add_harness(laiue_rigid_solver_test RigidSolverTestEntryPoint rigid_solver_test.c
    LIBRARIES laiue_physics laiue_numeric TEST laiue.physics.rigid_solver)
laiue_add_harness(laiue_rigid_cache_test RigidCacheTestEntryPoint rigid_cache_test.c
    LIBRARIES laiue_physics laiue_numeric TEST laiue.physics.rigid_cache)
laiue_add_harness(laiue_rigid_broadphase_test RigidBroadphaseTestEntryPoint
    rigid_broadphase_test.c LIBRARIES laiue_physics laiue_numeric
    TEST laiue.physics.rigid_broadphase)
laiue_add_harness(laiue_rigid_broadphase_scale_test RigidBroadphaseScaleTestEntryPoint
    rigid_broadphase_scale_test.c LIBRARIES laiue_physics laiue_numeric
    TEST laiue.physics.rigid_broadphase_scale TIMEOUT 60)
laiue_add_harness(laiue_rigid_parallel_test RigidParallelTestEntryPoint rigid_parallel_test.c
    LIBRARIES laiue::physics laiue::numeric laiue::task
    TEST laiue.physics.rigid_parallel TIMEOUT 60)
laiue_add_harness(laiue_rigid_compound_test RigidCompoundTestEntryPoint rigid_compound_test.c
    LIBRARIES laiue::physics laiue::numeric laiue::task
    TEST laiue.physics.rigid_compound TIMEOUT 120)
laiue_add_harness(laiue_compound_shape_test CompoundShapeTestEntryPoint compound_shape_test.c
    LIBRARIES laiue::physics TEST laiue.physics.compound_shape TIMEOUT 30)
laiue_add_harness(laiue_compound_overlap_test CompoundOverlapTestEntryPoint
    compound_overlap_test.c LIBRARIES laiue::physics
    TEST laiue.physics.compound_overlap TIMEOUT 60)
# The private spatial helper is tested directly; it is not part of the SDK ABI.
laiue_add_harness(laiue_compound_bvh_test CompoundBvhTestEntryPoint compound_bvh_test.c
    "${laiue_src}/simulation/physics/compound_bvh.c" PRECISE_FP LIBRARIES laiue::physics
    TEST laiue.physics.compound_bvh TIMEOUT 30 PROPERTIES RUN_SERIAL TRUE)
laiue_add_harness(laiue_ragdoll_test RagdollTestEntryPoint ragdoll_test.c
    LIBRARIES laiue_walk_common laiue::physics laiue::numeric laiue::math_support
    TEST laiue.physics.ragdoll TIMEOUT 30)
if(TARGET laiue_physics AND laiue_shared_modules)
    laiue_add_harness(laiue_physics_module_test PhysicsModuleTestEntryPoint
        physics_module_test.c BOOTSTRAP DEPENDS laiue_physics laiue_numeric laiue_task
        TEST laiue.physics.module)
endif()

# Content, modules and mods.
laiue_add_harness(laiue_content_format_test ContentFormatTestEntryPoint content_format_test.c
    LIBRARIES laiue_content TEST laiue.content.format)
laiue_add_harness(laiue_content_catalog_test ContentCatalogTestEntryPoint
    content_catalog_test.c LIBRARIES laiue_content TEST laiue.content.catalog)
laiue_add_harness(laiue_mod_manifest_test ModManifestTestEntry mod_manifest_test.c
    LIBRARIES laiue_mod TEST laiue.mod.manifest)
laiue_add_harness(laiue_mod_manifest_scan_test ModManifestScanTestEntry
    mod_manifest_scan_test.c LIBRARIES laiue_mod TEST laiue.mod.manifest_scan)
laiue_add_harness(laiue_module_manifest_test ModuleManifestTestEntryPoint
    module_manifest_test.c BOOTSTRAP TEST laiue.mod.module_manifest)
if(LAIUE_NATIVE_MODS_ENABLED)
    laiue_add_harness(laiue_numeric_module_test NumericModuleTestEntryPoint
        numeric_module_test.c BOOTSTRAP DEPENDS laiue_numeric TEST laiue.mod.numeric_module)
    laiue_add_harness(laiue_task_module_test TaskModuleTestEntryPoint task_module_test.c
        BOOTSTRAP DEPENDS laiue_task TEST laiue.mod.task_module)
    laiue_add_harness(laiue_content_module_test ContentModuleTestEntryPoint
        content_module_test.c BOOTSTRAP DEPENDS laiue_content TEST laiue.mod.content_module)
    laiue_add_harness(laiue_character_module_test CharacterModuleTestEntryPoint
        character_module_test.c BOOTSTRAP LIBRARIES laiue::math_support
        DEPENDS laiue_character TEST laiue.mod.character_module)
    laiue_add_harness(laiue_voxel_module_test VoxelModuleTestEntryPoint voxel_module_test.c
        BOOTSTRAP DEPENDS laiue_voxel TEST laiue.mod.voxel_module)

    laiue_add_test_module(laiue_mod_test_extension mod_test_extension.c mod_test_service.h)
    laiue_add_harness(laiue_mod_host_test ModHostTestEntry mod_host_test.c mod_test_service.h
        LIBRARIES laiue_mod DEPENDS laiue_mod_test_extension TEST laiue.mod.host)
    laiue_add_harness(laiue_mod_host_order_test ModHostOrderTestEntry mod_host_order_test.c
        mod_test_service.h LIBRARIES laiue_mod DEPENDS laiue_mod_test_extension
        TEST laiue.mod.host_order)

    # Two modules that link no LAIUE runtime: the consumer starts only after
    # the provider publishes its service, which covers ordering, explicit
    # loading, service lookup and reverse teardown.
    laiue_add_test_module(laiue_module_provider module_provider.c)
    laiue_add_test_module(laiue_module_consumer module_consumer.c)
    laiue_add_harness(laiue_module_host_test ModuleHostTestEntryPoint module_host_test.c
        BOOTSTRAP DEPENDS laiue_module_provider laiue_module_consumer
        TEST laiue.mod.module_host)

    if(TARGET laiue_audio AND TARGET laiue_ui AND TARGET laiue_voxel_render AND
       TARGET laiue_window AND TARGET laiue_input AND laiue_shared_modules)
        set(laiue_optional_definitions)
        if(LAIUE_RENDER_HAS_VULKAN_TARGET)
            list(APPEND laiue_optional_definitions LAIUE_TEST_HAS_VULKAN_RENDERER=1)
        endif()
        if(LAIUE_REQUIRE_VULKAN_TEST_DRIVER)
            list(APPEND laiue_optional_definitions LAIUE_TEST_REQUIRE_OFFSCREEN_DEVICE=1)
        endif()
        laiue_add_harness(laiue_optional_modules_test OptionalModulesTestEntryPoint
            optional_modules_test.c BOOTSTRAP DEFINITIONS ${laiue_optional_definitions}
            DEPENDS laiue_audio laiue_audio_pack laiue_ui laiue_input laiue_window
                laiue_voxel_render laiue_scene_math laiue_scene laiue_world laiue_mesher
                laiue_render laiue_numeric laiue_content
            TEST laiue.mod.optional_components)
    endif()

    # Every built module is removed, corrupted, and corrupted while pinned as a
    # provider; only its required-dependency closure may stop with it.
    if(laiue_shared_modules)
        get_property(laiue_matrix_runtime_targets GLOBAL PROPERTY LAIUE_RUNTIME_TARGETS)
        set(laiue_matrix_dependencies)
        foreach(matrix_target IN LISTS laiue_matrix_runtime_targets
                ITEMS laiue_graphics_d3d12 laiue_graphics_vulkan)
            if(TARGET ${matrix_target})
                list(APPEND laiue_matrix_dependencies ${matrix_target})
            endif()
        endforeach()
        laiue_add_harness(laiue_module_removal_matrix_test ModuleRemovalMatrixTestEntryPoint
            module_removal_matrix_test.c BOOTSTRAP DEPENDS ${laiue_matrix_dependencies}
            TEST laiue.mod.removal_matrix)
    endif()
else()
    laiue_add_harness(laiue_mod_host_disabled_test ModHostDisabledTestEntry
        mod_host_disabled_test.c LIBRARIES laiue_mod TEST laiue.mod.host_disabled)
endif()

# Meshing, voxel streaming and scene.
if(TARGET laiue_mesher)
    laiue_add_harness(laiue_chunk_mesher_test ChunkMesherTestEntryPoint chunk_mesher_test.c
        LIBRARIES laiue::mesher laiue::world laiue::numeric
        TEST laiue.mesh.chunk_mesher TIMEOUT 120)
    if(laiue_shared_modules)
        laiue_add_harness(laiue_mesher_module_test MesherModuleTestEntryPoint
            mesher_module_test.c BOOTSTRAP DEPENDS laiue_mesher TEST laiue.mesh.module)
    endif()
endif()
if(TARGET laiue_voxel_raycast)
    laiue_add_harness(laiue_voxel_raycast_core_test VoxelRaycastCoreTestEntryPoint
        voxel_raycast_core_test.c LIBRARIES laiue_voxel_raycast laiue_world laiue_numeric
        TEST laiue.voxel.raycast_core)
    if(laiue_shared_modules)
        laiue_add_harness(laiue_voxel_raycast_module_test VoxelRaycastModuleTestEntryPoint
            voxel_raycast_module_test.c BOOTSTRAP DEPENDS laiue_voxel_raycast
            TEST laiue.voxel.raycast_module)
    endif()
endif()
if(TARGET laiue_voxel_render)
    if(TARGET laiue_voxel_raycast)
        laiue_add_harness(laiue_voxel_raycast_test VoxelRaycastTestEntryPoint
            voxel_raycast_test.c
            LIBRARIES laiue_voxel_raycast laiue_voxel_render laiue_world laiue_mesher laiue_numeric
            TEST laiue.voxel.render_raycast)
    endif()
    laiue_add_harness(laiue_voxel_raycast_dda_test VoxelRaycastDdaTestEntryPoint
        voxel_raycast_dda_test.c LIBRARIES laiue_voxel_raycast laiue_world laiue_numeric
        TEST laiue.voxel.raycast_dda)
    # Случайные переходы и блуждание по кольцу сверяются с независимым
    # эталоном множества ключей и списком отрисовки.
    laiue_add_harness(laiue_chunk_streaming_stress_test ChunkStreamingStressTestEntryPoint
        chunk_streaming_stress_test.c
        LIBRARIES laiue_voxel_render laiue_world laiue_mesher laiue_numeric
        TEST laiue.voxel.render_chunk_streaming_stress TIMEOUT 120)

    if(TARGET laiue_world AND TARGET laiue_numeric AND TARGET laiue_mesher)
        # Platform faults are injected only into copies of the production bodies.
        configure_file("${laiue_src}/graphics/voxel_render/chunk_streaming.c"
            "${CMAKE_CURRENT_BINARY_DIR}/chunk_streaming_failure_subject.c" COPYONLY)
        configure_file("${laiue_src}/graphics/mesh/chunk_mesher.c"
            "${CMAKE_CURRENT_BINARY_DIR}/chunk_streaming_failure_mesher.c" COPYONLY)
        add_library(laiue_chunk_streaming_failure_subject OBJECT
            "${CMAKE_CURRENT_BINARY_DIR}/chunk_streaming_failure_subject.c"
            "${CMAKE_CURRENT_BINARY_DIR}/chunk_streaming_failure_mesher.c")
        target_link_libraries(laiue_chunk_streaming_failure_subject PRIVATE laiue_common)
        laiue_configure_precise_fp(laiue_chunk_streaming_failure_subject)
        target_compile_definitions(laiue_chunk_streaming_failure_subject PRIVATE
            LAIUE_STATIC=1
            PlatformAllocate=StreamingTestAllocate PlatformReallocate=StreamingTestReallocate
            PlatformFree=StreamingTestFree PlatformMutexInitialize=StreamingTestMutexInitialize
            PlatformMutexDestroy=StreamingTestMutexDestroy
            PlatformConditionVariableInitialize=StreamingTestConditionInitialize
            PlatformConditionVariableDestroy=StreamingTestConditionDestroy
            PlatformThreadStart=StreamingTestThreadStart PlatformThreadJoin=StreamingTestThreadJoin)
        laiue_add_harness(laiue_chunk_streaming_failure_test ChunkStreamingFailureTestEntryPoint
            chunk_streaming_failure_test.c $<TARGET_OBJECTS:laiue_chunk_streaming_failure_subject>
            STATIC LIBRARIES laiue_world laiue_numeric
            TEST laiue.voxel.render_chunk_streaming_failure TIMEOUT 40)
    endif()
    laiue_add_harness(laiue_panorama_test PanoramaTestEntryPoint panorama_test.c
        LIBRARIES laiue_scene laiue_scene_math TEST laiue.scene.panorama)
endif()

# Graphics. Driver-dependent tests skip without a GPU unless it is required.
if(TARGET laiue_render AND laiue_shared_modules)
    if(LAIUE_RENDER_HAS_VULKAN_TARGET)
        laiue_add_harness(laiue_graphics_geometry_vulkan_test GraphicsGeometryTestEntryPoint
            graphics_geometry_test.c BOOTSTRAP DEPENDS laiue_render
            DEFINITIONS LAIUE_GRAPHICS_TEST_D3D12=0
                "LAIUE_GRAPHICS_TEST_REQUIRE_DRIVER=$<BOOL:${LAIUE_REQUIRE_VULKAN_TEST_DRIVER}>"
            TEST laiue.graphics.geometry_vulkan TIMEOUT 90)
        laiue_add_harness(laiue_graphics_diagnostics_test GraphicsDiagnosticsTestEntryPoint
            graphics_diagnostics_test.c BOOTSTRAP DEPENDS laiue_render
            TEST laiue.graphics.diagnostics TIMEOUT 60)
        laiue_set_gpu_test_properties(${LAIUE_REQUIRE_VULKAN_TEST_DRIVER}
            laiue.graphics.geometry_vulkan laiue.graphics.diagnostics)
        laiue_add_vulkan_validation(laiue.graphics.geometry_vulkan)
    endif()
    if(LAIUE_RENDER_HAS_D3D12 AND WIN32)
        laiue_add_harness(laiue_graphics_geometry_d3d12_test GraphicsGeometryTestEntryPoint
            graphics_geometry_test.c BOOTSTRAP LIBRARIES user32 DEPENDS laiue_render
            DEFINITIONS LAIUE_GRAPHICS_TEST_D3D12=1
                "LAIUE_GRAPHICS_TEST_REQUIRE_DRIVER=$<BOOL:${LAIUE_REQUIRE_D3D12_TEST_DRIVER}>"
            TEST laiue.graphics.geometry_d3d12 TIMEOUT 90)
        laiue_set_gpu_test_properties(${LAIUE_REQUIRE_D3D12_TEST_DRIVER}
            laiue.graphics.geometry_d3d12)
    endif()
    if(TARGET laiue_graphics_profile)
        laiue_add_harness(laiue_graphics_profile_module_test GraphicsProfileModuleTestEntryPoint
            graphics_profile_module_test.c BOOTSTRAP DEPENDS laiue_graphics_profile
            TEST laiue.graphics.profile_module)
    endif()
endif()
if(TARGET laiue_render)
    # The rolling GPU profile is backend-independent and needs no GPU.
    laiue_add_harness(laiue_graphics_profile_test GraphicsProfileTestEntryPoint
        graphics_profile_test.c "${laiue_src}/graphics/profile/profile.c"
        STATIC INCLUDES "${laiue_src}" TEST laiue.graphics.profile)
    laiue_add_harness(laiue_render_pack_api_test RenderPackApiTestEntryPoint
        render_pack_api_test.c LIBRARIES laiue_content laiue_render TEST laiue.render.packs)
    # The internal two-pass pack builder is compiled into the test, so it runs
    # without a GPU or new exports. Its scenario functions are noinline: Release
    # LTO must not flatten them into a __chkstk-only no-CRT entry point.
    laiue_add_harness(laiue_texture_pack_build_test TexturePackBuildTestEntryPoint
        texture_pack_build_test.c texc_fixtures.h
        "${laiue_src}/graphics/render/content_provider.c"
        "${laiue_src}/graphics/render/texture_build.c"
        INCLUDES "${laiue_src}" DEFINITIONS LAIUE_BUILD_RENDER=1
        LIBRARIES laiue_content laiue::media_support TEST laiue.render.texture_build)
endif()
laiue_add_harness(laiue_texture_pack_equivalence_test TexturePackEquivalenceTestEntryPoint
    texture_pack_equivalence_test.c
    "${laiue_src}/graphics/render/content_provider.c"
    "${laiue_src}/graphics/render/texture_pack.c"
    "${laiue_src}/graphics/render/texture_build.c"
    STATIC INCLUDES "${laiue_src}" LIBRARIES laiue_content laiue::media_support
    TEST laiue.render.texture_pack_equiv)
laiue_add_harness(laiue_shader_manifest_test ShaderManifestTestEntryPoint
    shader_manifest_test.c
    "${laiue_src}/graphics/render/content_provider.c"
    "${laiue_src}/graphics/render/shader_pack.c"
    STATIC INCLUDES "${laiue_src}" LIBRARIES laiue_content
    TEST laiue.render.shader_manifest)
if(TARGET laiue_render AND LAIUE_RENDER_HAS_VULKAN_TARGET)
    laiue_add_harness(laiue_renderer_offscreen_test RendererOffscreenTestEntryPoint
        renderer_offscreen_test.c texc_fixtures.h LIBRARIES laiue_content laiue_render
        TEST laiue.render.offscreen_frame)
    laiue_add_harness(laiue_renderer_vulkan_instance_ring_test
        RendererVulkanInstanceRingTestEntryPoint renderer_vulkan_instance_ring_test.c
        LIBRARIES laiue_render laiue_content TEST laiue.render.vulkan_instance_ring TIMEOUT 180)
    laiue_set_gpu_test_properties(${LAIUE_REQUIRE_VULKAN_TEST_DRIVER}
        laiue.render.offscreen_frame laiue.render.vulkan_instance_ring)
    # Hidden windows only; a missing driver or surface is always a skip.
    laiue_add_harness(laiue_renderer_vulkan_lifecycle_test RendererVulkanLifecycleTestEntryPoint
        renderer_vulkan_lifecycle_test.c LIBRARIES laiue_render laiue_content
        TEST laiue.render.vulkan_lifecycle TIMEOUT 180)
    laiue_set_gpu_test_properties(OFF laiue.render.vulkan_lifecycle)
    laiue_add_vulkan_validation(laiue.render.offscreen_frame
        laiue.render.vulkan_instance_ring laiue.render.vulkan_lifecycle)
endif()
if(TARGET laiue_render AND LAIUE_RENDER_HAS_D3D12)
    if(LAIUE_RENDER_HAS_VULKAN_TARGET)
        # Both backends alternate frames in one process.
        laiue_add_harness(laiue_render_dual_backend_test RenderDualBackendTestEntryPoint
            render_dual_backend_test.c LIBRARIES laiue_render laiue_content
            TEST laiue.render.dual_backend TIMEOUT 60)
        laiue_set_gpu_test_properties(OFF laiue.render.dual_backend)
    endif()
    laiue_add_harness(laiue_renderer_instance_ring_test RendererInstanceRingTestEntryPoint
        renderer_instance_ring_test.c LIBRARIES laiue_render laiue_content
        TEST laiue.render.instance_ring TIMEOUT 60 PROPERTIES SKIP_RETURN_CODE 125)
endif()
if(TARGET laiue_ui)
    laiue_add_harness(laiue_ui_test UiTestEntryPoint ui_test.c LIBRARIES laiue_ui
        TEST laiue.ui.draw_list)
endif()

# Audio. The offscreen backend needs no sound device.
if(TARGET laiue_audio)
    laiue_add_harness(laiue_audio_api_test AudioApiTestEntryPoint audio_api_test.c
        LIBRARIES laiue_audio TEST laiue.audio.mixer)
    laiue_add_harness(laiue_audio_stream_test AudioStreamTestEntryPoint audio_stream_test.c
        LIBRARIES laiue_audio TEST laiue.audio.streaming TIMEOUT 60)
    if(TARGET laiue_audio_pack AND TARGET laiue_content AND laiue_shared_modules)
        laiue_add_harness(laiue_audio_pack_test AudioPackTestEntryPoint
            audio_pack_test.c mp3_fixtures.h BOOTSTRAP
            DEPENDS laiue_audio_pack laiue_audio laiue_content TEST laiue.audio.packs)
    endif()
    # System output is checked silently; no sound device means a skip.
    if(TARGET laiue_audio_output AND laiue_shared_modules)
        laiue_add_harness(laiue_audio_system_backend_test AudioSystemBackendTestEntryPoint
            audio_system_backend_test.c BOOTSTRAP DEPENDS laiue_audio laiue_audio_output
            TEST laiue.audio.system_backend TIMEOUT 30
            PROPERTIES SKIP_RETURN_CODE 125 RUN_SERIAL TRUE)
        # ALSA is loaded at runtime; recreating the output must not leak.
        if(LAIUE_PLATFORM_POSIX AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
            laiue_add_harness(laiue_audio_output_cycle_test AudioOutputCycleTestEntryPoint
                audio_output_cycle_test.c BOOTSTRAP DEPENDS laiue_audio_output
                TEST laiue.audio.output_cycle TIMEOUT 60)
        endif()
    endif()
endif()

# Decoders, encoders and content tools.
if(TARGET laiue_media_support)
    set(laiue_media laiue::media_support)
    laiue_add_harness(laiue_image_decode_test ImageDecodeTestEntryPoint
        image_decode_test.c texc_fixtures.h LIBRARIES ${laiue_media}
        TEST laiue.media.image_decode)
    laiue_add_harness(laiue_gif_decode_test GifDecodeTestEntryPoint
        gif_decode_test.c gif_fixtures.h texc_fixtures.h LIBRARIES ${laiue_media}
        TEST laiue.media.gif_decode)
    laiue_add_harness(laiue_image_resample_test ImageResampleTestEntryPoint
        image_resample_test.c LIBRARIES ${laiue_media} TEST laiue.media.resample)
    laiue_add_harness(laiue_image_resample_general_test ImageResampleGeneralTestEntryPoint
        image_resample_general_test.c LIBRARIES ${laiue_media}
        TEST laiue.media.resample_general)
    laiue_add_harness(laiue_inflate_test InflateTestEntryPoint inflate_test.c inflate_fixtures.h
        LIBRARIES ${laiue_media} TEST laiue.media.inflate)
    laiue_add_harness(laiue_inflate_backref_test InflateBackrefTestEntryPoint
        inflate_backref_test.c inflate_backref_fixtures.h LIBRARIES ${laiue_media}
        TEST laiue.media.inflate_backref)
    laiue_add_harness(laiue_mp3_test Mp3TestEntryPoint mp3_test.c mp3_fixtures.h
        LIBRARIES ${laiue_media} TEST laiue.media.mp3)
    laiue_add_harness(laiue_audio_stream_decoder_test AudioStreamDecoderTestEntryPoint
        audio_stream_decoder_test.c LIBRARIES ${laiue_media} TEST laiue.media.sound_stream)
    laiue_add_harness(laiue_la_encode_test LaEncodeTestEntryPoint la_encode_test.c
        LIBRARIES ${laiue_media} TEST laiue.media.la_encode)
    laiue_add_harness(laiue_model_codec_test ModelCodecTestEntryPoint model_codec_test.c
        LIBRARIES ${laiue_media} TEST laiue.media.model_codec)
    laiue_add_harness(laiue_animation_codec_test AnimationCodecTestEntryPoint
        animation_codec_test.c LIBRARIES ${laiue_media} TEST laiue.media.animation_codec)
    laiue_add_harness(laiue_texc_test TexcTestEntryPoint texc_test.c texc_fixtures.h
        LIBRARIES ${laiue_media} TEST laiue.tools.texc)
    laiue_add_harness(laiue_soundc_test SoundcTestEntryPoint soundc_test.c
        LIBRARIES ${laiue_media} TEST laiue.tools.soundc)
    if(TARGET laiue_modelc)
        add_test(NAME laiue.tools.modelc
            COMMAND "${CMAKE_COMMAND}"
                "-DMODELC=$<TARGET_FILE:laiue_modelc>"
                "-DINPUT_OBJ=${CMAKE_CURRENT_SOURCE_DIR}/fuzz/corpus/model/cube.obj"
                "-DWORK_DIRECTORY=${CMAKE_CURRENT_BINARY_DIR}/modelc_test"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/run_modelc_test.cmake")
    endif()
    if(TARGET laiue_model AND TARGET laiue_content AND laiue_shared_modules)
        laiue_add_harness(laiue_model_pack_test ModelPackTestEntryPoint model_pack_test.c
            BOOTSTRAP LIBRARIES laiue_model ${laiue_media}
            DEPENDS laiue_model laiue_content TEST laiue.model.packs)
    endif()
endif()

# Models and animation.
if(TARGET laiue_model)
    laiue_add_harness(laiue_model_skin_test ModelSkinTestEntryPoint model_skin_test.c
        PRECISE_FP LIBRARIES laiue_model laiue::media_support TEST laiue.model.skin)
    laiue_add_harness(laiue_model_skin_alias_test ModelSkinAliasTestEntryPoint
        model_skin_alias_test.c PRECISE_FP LIBRARIES laiue::media_support
        TEST laiue.model.skin_alias)
endif()
if(TARGET laiue_animation)
    laiue_add_harness(laiue_animation_test AnimationTestEntryPoint animation_test.c
        LIBRARIES laiue_animation laiue::media_support TEST laiue.animation.sampling)
    laiue_add_harness(laiue_animation_scratch_test AnimationScratchTestEntryPoint
        animation_scratch_test.c PRECISE_FP LIBRARIES laiue_animation
        TEST laiue.animation.scratch)
    if(TARGET laiue_content AND laiue_shared_modules)
        laiue_add_harness(laiue_animation_pack_test AnimationPackTestEntryPoint
            animation_pack_test.c BOOTSTRAP
            LIBRARIES laiue_animation laiue_content laiue::media_support
            DEPENDS laiue_content TEST laiue.animation.packs)
    endif()
endif()
if(TARGET laiue_animation_demo AND TARGET laiue_animation AND TARGET laiue_model)
    add_test(NAME laiue.animation.demo_cpu COMMAND laiue_animation_demo)
    set_tests_properties(laiue.animation.demo_cpu PROPERTIES TIMEOUT 30
        ENVIRONMENT "LAIUE_ANIMATION_DEMO_MODE=cpu;LAIUE_ANIMATION_REQUIRE_FULL=1")
    laiue_add_harness(laiue_animation_model_test AnimationModelTestEntryPoint
        animation_model_test.c PRECISE_FP
        LIBRARIES laiue_animation laiue_model laiue::math_support
        DEPENDS laiue_animation_demo_assets TEST laiue.animation.model_binding)
    foreach(backend IN ITEMS vulkan d3d12)
        if(backend STREQUAL "vulkan" AND LAIUE_RENDER_HAS_VULKAN_TARGET)
            set(require_driver "${LAIUE_REQUIRE_VULKAN_TEST_DRIVER}")
        elseif(backend STREQUAL "d3d12" AND WIN32 AND LAIUE_RENDER_HAS_D3D12)
            set(require_driver "${LAIUE_REQUIRE_D3D12_TEST_DRIVER}")
        else()
            continue()
        endif()
        set(demo_test "laiue.animation.demo_${backend}")
        add_test(NAME ${demo_test} COMMAND laiue_animation_demo)
        set_tests_properties(${demo_test} PROPERTIES TIMEOUT 90
            ENVIRONMENT "LAIUE_ANIMATION_DEMO_MODE=capture;LAIUE_ANIMATION_REQUIRE_FULL=1;LAIUE_ANIMATION_DEMO_BACKEND=${backend};LAIUE_ANIMATION_REQUIRE_DRIVER=$<BOOL:${require_driver}>")
        # The demo enforces the driver requirement itself and reports 125 otherwise.
        laiue_set_gpu_test_properties(OFF ${demo_test})
        if(backend STREQUAL "vulkan")
            laiue_add_vulkan_validation(${demo_test})
        endif()
    endforeach()
endif()

# Infinite world of models.
if(TARGET laiue_mesh_world)
    laiue_add_harness(laiue_mesh_world_test MeshWorldTestEntryPoint mesh_world_test.c
        BOOTSTRAP LIBRARIES laiue_mesh_world TEST laiue.mesh_world.queries)
endif()
if(TARGET laiue_mesh_world_render AND TARGET laiue_mesh_world)
    laiue_add_harness(laiue_mesh_world_render_test MeshWorldRenderTestEntryPoint
        mesh_world_render_test.c LIBRARIES laiue_mesh_world_render laiue_mesh_world
        TEST laiue.mesh_world.render)
    # The same implementation compiled with a test allocator; production keeps
    # the ordinary platform ABI.
    configure_file("${laiue_src}/graphics/mesh_world_render/mesh_world_render.c"
        "${CMAKE_CURRENT_BINARY_DIR}/mesh_world_render_failure_subject.c" COPYONLY)
    add_library(laiue_mesh_world_render_failure_subject OBJECT
        "${CMAKE_CURRENT_BINARY_DIR}/mesh_world_render_failure_subject.c")
    target_link_libraries(laiue_mesh_world_render_failure_subject PRIVATE
        laiue_common laiue::math_support)
    target_compile_definitions(laiue_mesh_world_render_failure_subject PRIVATE
        LAIUE_STATIC=1 PlatformAllocate=RendererTestAllocate
        PlatformReallocate=RendererTestReallocate PlatformFree=RendererTestFree)
    laiue_add_harness(laiue_mesh_world_render_failure_test MeshWorldRenderFailureTestEntryPoint
        mesh_world_render_failure_test.c
        $<TARGET_OBJECTS:laiue_mesh_world_render_failure_subject>
        STATIC LIBRARIES laiue_mesh_world laiue::math_support
        TEST laiue.mesh_world.render_failure)
endif()
if(TARGET laiue_mesh_world_demo_common AND TARGET laiue_mesh_world)
    laiue_add_harness(laiue_mesh_world_demo_world_test MeshWorldDemoWorldTestEntryPoint
        mesh_world_demo_world_test.c BOOTSTRAP
        LIBRARIES laiue_mesh_world_demo_common laiue_mesh_world
        TEST laiue.mesh_world.demo_world)
endif()
if(TARGET laiue_mesh_world_demo AND TARGET laiue_model AND TARGET laiue_content
   AND TARGET laiue_mesh_world AND TARGET laiue_mesh_world_render)
    add_test(NAME laiue.mesh_world.demo_headless COMMAND laiue_mesh_world_demo --headless 12)
    set_tests_properties(laiue.mesh_world.demo_headless PROPERTIES TIMEOUT 60)
endif()

# Walk example.
laiue_add_harness(laiue_walk_runtime_test WalkRuntimeTestEntryPoint walk_runtime_test.c
    LIBRARIES laiue_walk_common INCLUDES "${laiue_src}" "${laiue_walk_example}"
    TEST laiue.example.walk_runtime)
laiue_add_harness(laiue_walk_gameplay_test WalkGameplayTestEntryPoint walk_gameplay_test.c
    LIBRARIES laiue_walk_common laiue::physics laiue::numeric
    INCLUDES "${laiue_walk_example}" TEST laiue.example.walk_gameplay)
laiue_add_harness(laiue_walk_scenario_test WalkScenarioTestEntryPoint walk_scenario_test.c
    LIBRARIES laiue_walk_scenario_support INCLUDES "${laiue_walk_example}"
    TEST laiue.example.walk_scenario)
laiue_add_harness(laiue_walk_world_test WalkWorldTestEntryPoint walk_world_test.c
    LIBRARIES laiue_walk_common laiue::math_support INCLUDES "${laiue_walk_example}"
    TEST laiue.example.walk_world)
if(TARGET laiue_media_support)
    laiue_add_harness(laiue_walk_visuals_test WalkVisualsTestEntryPoint walk_visuals_test.c
        LIBRARIES laiue_walk_common laiue::physics laiue::numeric laiue::math_support
            laiue::media_support
        INCLUDES "${laiue_walk_example}" TEST laiue.example.walk_visuals)
endif()
if(TARGET laiue_frame_check)
    add_test(NAME laiue.tools.frame_check
        COMMAND laiue_frame_check "${PROJECT_SOURCE_DIR}/examples/walk/android/assets/textures/grass.png")
    add_test(NAME laiue.tools.frame_check.reject_text
        COMMAND laiue_frame_check "${PROJECT_SOURCE_DIR}/README.md")
    set_tests_properties(laiue.tools.frame_check.reject_text PROPERTIES WILL_FAIL TRUE)
endif()

# ASan/UBSan slow compute-heavy tests several times over; scale the limits so
# a timeout still means a hang rather than instrumentation overhead.
if(LAIUE_ENABLE_SANITIZERS OR LAIUE_BUILD_FUZZERS)
    get_property(laiue_sanitized_tests DIRECTORY PROPERTY TESTS)
    foreach(sanitized_test IN LISTS laiue_sanitized_tests)
        get_test_property(${sanitized_test} TIMEOUT sanitized_timeout)
        if(sanitized_timeout)
            math(EXPR sanitized_timeout "${sanitized_timeout} * 4")
            set_tests_properties(${sanitized_test} PROPERTIES TIMEOUT ${sanitized_timeout})
        endif()
    endforeach()
endif()
