laiue_add_module(input
    RUNTIME_ROLE GRAPHICS
    SOURCES
        input_windows.c
        input.h
        ../api.h
    LINK user32
)
