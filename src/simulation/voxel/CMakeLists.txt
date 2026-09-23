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
)

if(LAIUE_MODULE_LIBRARY_TYPE_RESOLVED STREQUAL "STATIC")
    target_link_libraries(laiue_voxel_raycast PRIVATE laiue_world)
else()
    target_compile_definitions(laiue_voxel_raycast PRIVATE
        LAIUE_VOXEL_RAYCAST_NO_WORLD_IMPORT=1)
endif()
