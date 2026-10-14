# Manual benchmarks: built on request, never part of ALL or CTest. Each one
# prints its timings and a checksum that must not change between builds.
set(laiue_src "${PROJECT_SOURCE_DIR}/src")

# Simulation.
laiue_add_harness(laiue_world_benchmark WorldReadBenchmarkEntryPoint world_read_benchmark.c
    LIBRARIES laiue_world laiue_numeric)
laiue_add_harness(laiue_world_batch_benchmark WorldBatchBenchmarkEntryPoint world_batch_benchmark.c
    LIBRARIES laiue_world laiue_numeric)
laiue_add_harness(laiue_physics_benchmark PhysicsBenchmarkEntryPoint physics_benchmark.c
    LIBRARIES laiue_physics laiue_numeric)
laiue_add_harness(laiue_numeric_physics_benchmark NumericPhysicsBenchmarkEntryPoint
    numeric_physics_benchmark.c LIBRARIES laiue_numeric)
laiue_add_harness(laiue_voxel_body_benchmark VoxelBodyBenchmarkEntryPoint voxel_body_benchmark.c
    LIBRARIES laiue_physics)
laiue_add_harness(laiue_rigid_solver_profile RigidSolverProfileEntryPoint rigid_solver_profile.c
    LIBRARIES laiue_physics laiue_numeric)
laiue_add_harness(laiue_task_pool_benchmark TaskPoolBenchmarkEntryPoint task_pool_benchmark.c
    LIBRARIES laiue::task)
# Private physics helpers are compiled into the benchmark instead of exported.
laiue_add_harness(laiue_broadphase_benchmark BroadphaseBenchmarkEntryPoint broadphase_benchmark.c
    "${laiue_src}/simulation/physics/rigid_broadphase.c" STATIC INCLUDES "${laiue_src}")
laiue_add_harness(laiue_compound_benchmark CompoundBenchmarkEntryPoint compound_benchmark.c
    "${laiue_src}/simulation/physics/compound_bvh.c" PRECISE_FP LIBRARIES laiue::physics)
if(TARGET laiue_walk_common)
    laiue_add_harness(laiue_ragdoll_benchmark RagdollBenchmarkEntryPoint ragdoll_benchmark.c
        LIBRARIES laiue_walk_common laiue_physics laiue_numeric laiue::math_support)
endif()
if(TARGET laiue_mesh_world)
    laiue_add_harness(laiue_mesh_world_query_benchmark MeshWorldQueryBenchmarkEntryPoint
        mesh_world_query_benchmark.c STATIC LIBRARIES laiue_mesh_world
        INCLUDES "${laiue_src}/simulation" "${laiue_src}/modding")
endif()

# Content, modules and mods.
laiue_add_harness(laiue_content_catalog_benchmark ContentCatalogBenchmarkEntryPoint
    content_catalog_benchmark.c LIBRARIES laiue_content)
if(TARGET laiue_mod)
    laiue_add_harness(laiue_mod_benchmark ModBenchmarkEntryPoint mod_benchmark.c
        LIBRARIES laiue_mod)
endif()
if(TARGET laiue_bootstrap)
    laiue_add_harness(laiue_module_host_benchmark ModuleHostBenchmarkEntryPoint
        module_host_benchmark.c BOOTSTRAP)
endif()
if(TARGET laiue_animation)
    laiue_add_harness(laiue_animation_benchmark AnimationBenchmarkEntryPoint animation_benchmark.c
        PRECISE_FP LIBRARIES laiue_animation)
endif()
if(TARGET laiue_media_support)
    laiue_add_harness(laiue_image_benchmark ImageBenchmarkEntryPoint image_benchmark.c
        LIBRARIES laiue::media_support)
    laiue_add_harness(laiue_image_resample_benchmark ImageResampleBenchmarkEntryPoint
        image_resample_benchmark.c LIBRARIES laiue::media_support)
    laiue_add_harness(laiue_png_decode_benchmark PngDecodeBenchmarkEntryPoint
        png_decode_benchmark.c INCLUDES "${laiue_src}" LIBRARIES laiue::media_support)
endif()

# Audio.
if(TARGET laiue_audio)
    laiue_add_harness(laiue_audio_mixer_benchmark AudioMixerBenchmarkEntryPoint
        audio_mixer_benchmark.c LIBRARIES laiue_audio)
    laiue_add_harness(laiue_audio_stream_benchmark AudioStreamBenchmarkEntryPoint
        audio_stream_benchmark.c LIBRARIES laiue_audio)
    laiue_add_harness(laiue_audio_pack_benchmark AudioPackBenchmarkEntryPoint
        audio_pack_benchmark.c LIBRARIES laiue_audio laiue_audio_pack laiue_mod laiue_content)
endif()

# Graphics.
if(TARGET laiue_mesher AND TARGET laiue_numeric)
    laiue_add_harness(laiue_mesher_benchmark MesherBenchmarkEntryPoint mesher_benchmark.c
        LIBRARIES laiue_mesher laiue_world laiue_numeric)
endif()
if(TARGET laiue_mesher AND TARGET laiue_audio)
    laiue_add_harness(laiue_benchmark EngineBenchmarkEntryPoint engine_benchmark.c
        LIBRARIES laiue_audio laiue_mesher laiue_world laiue_numeric)
endif()
if(TARGET laiue_voxel_render AND TARGET laiue_mesher)
    laiue_add_harness(laiue_streaming_benchmark StreamingBenchmarkEntryPoint streaming_benchmark.c
        LIBRARIES laiue_voxel_render laiue_world laiue_mesher laiue_numeric)
endif()
if(TARGET laiue_scene)
    laiue_add_harness(laiue_raycast_benchmark RaycastBenchmarkEntryPoint raycast_benchmark.c
        LIBRARIES laiue_scene laiue_scene_math laiue_world laiue_voxel_raycast laiue_numeric)
    laiue_add_harness(laiue_panorama_benchmark PanoramaBenchmarkEntryPoint panorama_benchmark.c
        LIBRARIES laiue_scene)
endif()
if(TARGET laiue_render)
    laiue_add_harness(laiue_renderer_submit_benchmark RendererSubmitBenchmarkEntryPoint
        renderer_submit_benchmark.c LIBRARIES laiue_render)
    laiue_add_harness(laiue_generic_descriptor_benchmark GenericDescriptorBenchmarkEntryPoint
        generic_descriptor_benchmark.c LIBRARIES laiue_render)
    if(TARGET laiue_media_support)
        laiue_add_harness(laiue_texture_build_benchmark TextureBuildBenchmarkEntryPoint
            texture_build_benchmark.c
            "${laiue_src}/graphics/render/content_provider.c"
            "${laiue_src}/graphics/render/texture_build.c"
            STATIC INCLUDES "${laiue_src}" LIBRARIES laiue_content laiue::media_support)
    endif()
    if(TARGET laiue_bootstrap AND LAIUE_RENDER_HAS_VULKAN_TARGET AND
       LAIUE_MODULE_LIBRARY_TYPE_RESOLVED STREQUAL "SHARED")
        laiue_add_harness(laiue_graphics_geometry_benchmark GraphicsGeometryBenchmarkEntryPoint
            graphics_geometry_benchmark.c BOOTSTRAP DEPENDS laiue_render)
        if(TARGET laiue_mesh_world AND TARGET laiue_mesh_world_render)
            laiue_add_harness(laiue_mesh_world_render_benchmark MeshWorldRenderBenchmarkEntryPoint
                mesh_world_render_benchmark.c BOOTSTRAP
                INCLUDES "${laiue_src}/graphics" "${laiue_src}/simulation" "${laiue_src}/modding"
                DEPENDS laiue_render laiue_mesh_world laiue_mesh_world_render)
        endif()
    endif()
endif()
