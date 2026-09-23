laiue_add_module(physics
    PRECISE_FP
    RUNTIME_ROLE CORE
    SOURCES
        compound_bvh.c
        compound_bvh.h
        compound_shape.c
        compound_shape.h
        numeric_provider.c
        numeric_provider.h
        fp_environment.c
        fp_environment.h
        rigid_broadphase.c
        rigid_broadphase.h
        rigid_body.c
        rigid_body.h
        voxel_body.c
        voxel_body.h
        physics_service.h
        module_entry.c
        ../../api.h
    LINK
        laiue::math_support
)

# The voxel solver uses a precise, non-contracting FP model on supported x64
# toolchains. Applications decide how (or whether) to synchronize simulation.
