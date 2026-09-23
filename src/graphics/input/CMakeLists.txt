laiue_add_module(input
    RUNTIME_ROLE GRAPHICS
    SOURCES
        input_windows.c
        input.h
        input_service.h
        module_entry.c
        ../../api.h
    LINK user32
)
