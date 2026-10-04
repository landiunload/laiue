# Sampling and skinning are independent of graphics, models and world.
laiue_add_module(animation
    PRECISE_FP
    RUNTIME_ROLE CORE
    SOURCES
        animation_pack.c
        animation_sample.c
        animation_internal.h
        animation_service.h
        module_entry.c
        ../../api.h
    LINK
        laiue::media_support
)
