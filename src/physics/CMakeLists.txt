laiue_add_module(physics
    PRECISE_FP
    RUNTIME_ROLE CORE
    SOURCES
        fp_environment.c
        fp_environment.h
        voxel_body.c
        voxel_body.h
        ../api.h
)

# The voxel solver uses a precise, non-contracting FP model on supported x64
# toolchains. Applications decide how (or whether) to synchronize simulation.
