laiue_add_module(ui
    RUNTIME_ROLE CORE
    SOURCES
        ui.c
        ui.h
        ui_service.h
        module_entry.c
        ui_font.c
        ui_font.h
        ui_format.c
        ui_format.h
        ../api.h
    LINK
        laiue::math_support
    WINDOWS_LINK
        gdi32
)
