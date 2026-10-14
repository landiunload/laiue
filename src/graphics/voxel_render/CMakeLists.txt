laiue_add_module(voxel_render
    RUNTIME_ROLE GRAPHICS
    SOURCES
        chunk_streaming.c
        chunk_streaming.h
        module_entry.c
        voxel_render_service.h
        ../scene/math_service.h
        ../mesh/mesher_service.h
        ../render/graphics_service.h
        ../../simulation/world/world_service.h
        ../../api.h
)
