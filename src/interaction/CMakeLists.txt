laiue_add_module(interaction
    RUNTIME_ROLE HEADLESS
    SOURCES
        voxel_raycast.c
        voxel_raycast.h
        voxel_interaction.c
        voxel_interaction.h
        ../api.h
    LINK
        laiue_world
        laiue_physics
)
