laiue_add_module(mesher
    RUNTIME_ROLE GRAPHICS
    SOURCES
        chunk_mesher.c
        chunk_mesher.h
        mesher_service.h
        module_entry.c
        surface_mesher.c
        surface_mesher.h
        ../render/chunk_geometry.h
        ../../api.h
    LINK
        laiue::math_support
)
