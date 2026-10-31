laiue_add_module(input
    RUNTIME_ROLE GRAPHICS
    SOURCES
        input.h
        input_service.h
        input_state.c
        input_state.h
        module_entry.c
        ../../api.h
    LINK laiue::math_support
    WINDOWS_SOURCES input_windows.c
    WINDOWS_LINK user32
    POSIX_SOURCES input_x11.c
    POSIX_LINK X11::X11
    EXTERNAL_SOURCES input_android.c
    EXTERNAL_LINK android)
