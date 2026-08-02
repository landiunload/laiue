laiue_add_module(construct
    RUNTIME_ROLE HEADLESS
    SOURCES
        physical_construct.c
        physical_construct.h
        physical_construct_persistence.c
        physical_construct_persistence.h
        physical_construct_store.c
        physical_construct_store.h
        ../api.h
    LINK
        laiue_world
)

# Construct movement is part of the shared fixed-step simulation contract.
set_source_files_properties(
    physical_construct.c
    physical_construct_store.c
    PROPERTIES COMPILE_OPTIONS
    "${LAIUE_DETERMINISTIC_FP_OPTIONS}")
