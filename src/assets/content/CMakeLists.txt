laiue_add_module(content
    RUNTIME_ROLE CORE
    SOURCES
        content_catalog.c
        content_catalog.h
        content_service.h
        module_entry.c
        content_format.c
        content_format.h
        ../../api.h
    WINDOWS_LINK
        bcrypt
)
