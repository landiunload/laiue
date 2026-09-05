laiue_add_module(world
    RUNTIME_ROLE CORE
    SOURCES
        world_infinite.c
        world.h
        ../api.h
    LINK
        laiue::numeric
)
