laiue_add_module(voxel_render
    RUNTIME_ROLE GRAPHICS
    SOURCES
        chunk_streaming.c
        chunk_streaming.h
        module_entry.c
        voxel_render_service.h
        ../scene/math_service.h
        ../../api.h
    LINK
        laiue_world
        laiue_mesher
        laiue_render
)
