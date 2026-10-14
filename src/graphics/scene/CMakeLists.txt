laiue_add_module(scene_math
    RUNTIME_ROLE GRAPHICS
    SOURCES
        math.c
        math.h
        math_service.h
        math_module_entry.c
        ../../api.h
    LINK
        laiue::math_support
)

laiue_add_module(scene
    RUNTIME_ROLE GRAPHICS
    SOURCES
        camera.c
        camera.h
        math.h
        panorama.c
        panorama.h
        scene_service.h
        module_entry.c
        ../../api.h
    LINK laiue::math_support
)
