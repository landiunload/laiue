laiue_add_module(mesher
    SOURCES
        chunk_mesher.c
        chunk_mesher.h
        ../render/chunk_geometry.h
        ../api.h
    LINK laiue_world
)
