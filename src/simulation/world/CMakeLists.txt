laiue_add_module(world
    RUNTIME_ROLE CORE
    SOURCES
        world_infinite.c
        world.h
        world_service.h
        module_entry.c
        ../../api.h
    LINK
        laiue::numeric
)
