laiue_add_module(ui
    RUNTIME_ROLE GRAPHICS
    SOURCES
        ui.c
        ui.h
        ui_font.c
        ui_font.h
        ui_format.c
        ui_format.h
        ../api.h
    LINK
        laiue_render
        laiue_scene
    WINDOWS_LINK
        gdi32
)
