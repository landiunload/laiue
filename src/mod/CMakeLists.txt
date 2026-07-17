laiue_add_module(mod
    SOURCES
        mod_host.c
        mod_host.h
        mods.c
        mods.h
        ../api.h
    LINK
        bcrypt
        laiue_content
        laiue_world
        laiue_gameplay
)
target_include_directories(laiue_mod PRIVATE "${PROJECT_SOURCE_DIR}/sdk")
