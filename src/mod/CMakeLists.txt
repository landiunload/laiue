laiue_add_module(mod
    RUNTIME_ROLE HEADLESS
    SOURCES
        mod_block_policy.c
        mod_block_policy.h
        mod_host.c
        mod_host.h
        mods.c
        mods.h
        ../api.h
    LINK
        laiue_content
        laiue_world
        laiue_gameplay
)
target_include_directories(laiue_mod PRIVATE "${PROJECT_SOURCE_DIR}/sdk")
