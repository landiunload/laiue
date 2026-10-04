# Отрисовка мира на мешах через публичный графический контракт. Переносимый
# C без бэкенда: буферы создаются через LaiueGraphicsDeviceV2, который
# передаёт приложение, поэтому модуль собирается и в headless-профилях и
# тестируется подставным устройством.
laiue_add_module(mesh_world_render
    PRECISE_FP
    RUNTIME_ROLE CORE
    SOURCES
        mesh_world_render.c
        mesh_world_render_service.h
        module_entry.c
        ../../api.h
    LINK
        laiue::math_support
)
