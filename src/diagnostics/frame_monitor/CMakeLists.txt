# Диагностика: модуль собирается и ставится только в Debug и RelWithDebInfo.
laiue_add_module(frame_monitor
    RUNTIME_ROLE DIAGNOSTICS
    SOURCES
        frame_monitor.h
        frame_monitor.c
        frame_monitor_service.h
        module_entry.c
        ../../api.h
)
