laiue_add_module(persist
    RUNTIME_ROLE CORE
    SOURCES
        persist.h
        persist_service.h
        persist_internal.h
        persist_crc.c
        persist_crc.h
        persist_lz.c
        persist_lz.h
        persist_document.c
        persist_reader.c
        persist_save.c
        persist_job.c
        module_entry.c
        ../api.h
)
