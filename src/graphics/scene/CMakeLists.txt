laiue_add_module(scene_math
    RUNTIME_ROLE GRAPHICS
    SOURCES
        math.c
        math.h
        ../../api.h
    LINK
        laiue::math_support
)

laiue_add_module(scene
    RUNTIME_ROLE GRAPHICS
    SOURCES
        camera.c
        camera.h
        math.h
        panorama.c
        panorama.h
        ../../api.h
    LINK
        laiue_scene_math
        laiue::math_support
        laiue_render
)

laiue_add_module(scene_streaming
    RUNTIME_ROLE GRAPHICS
    SOURCES
        chunk_streaming.c
        chunk_streaming.h
        math.h
        ../../api.h
    LINK
        laiue_scene_math
        laiue_world
        laiue_mesher
        laiue_render
)

laiue_add_module(voxel_raycast
    RUNTIME_ROLE CORE
    SOURCES
        voxel_raycast.c
        voxel_raycast.h
        ../../api.h
    LINK
        laiue_world
)
