laiue_add_module(voxel
    RUNTIME_ROLE CORE
    SOURCES
        voxel_api.h
        voxel_service.h
        module_entry.c
)

laiue_add_module(voxel_raycast
    RUNTIME_ROLE CORE
    SOURCES
        voxel_raycast.c
        raycast.h
        raycast_module_entry.c
        raycast_service.h
        ../../api.h
    LINK
        laiue_world
)
