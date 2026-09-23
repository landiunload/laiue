laiue_add_module(input
    RUNTIME_ROLE GRAPHICS
    SOURCES
        input.h
        input_service.h
        module_entry.c
        ../../api.h
    WINDOWS_SOURCES input_windows.c
    WINDOWS_LINK user32
    POSIX_SOURCES input_x11.c
    POSIX_LINK X11::X11)
