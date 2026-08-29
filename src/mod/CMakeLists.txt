laiue_add_module(mod
    RUNTIME_ROLE CORE
    SOURCES
        mod.h
        mod_api.h
        mod_host.c
        mod_host.h
        mod_internal.c
        mod_internal.h
        mod_manifest.c
        mod_manifest.h
        mod_types.h
        ../api.h
    UNIX_LINK
        ${CMAKE_DL_LIBS}
)
