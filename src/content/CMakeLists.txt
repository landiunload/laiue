laiue_add_module(content
    RUNTIME_ROLE HEADLESS
    SOURCES
        content_catalog.c
        content_catalog.h
        content_bundle.c
        content_bundle.h
        content_format.c
        content_format.h
        ../api.h
    WINDOWS_LINK
        bcrypt
)
