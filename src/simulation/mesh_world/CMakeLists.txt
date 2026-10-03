# Бесконечный мир на мешах: размещение экземпляров моделей в разреженной
# сетке ячеек, запросы и столкновения с их коробками. Без графики, без
# загрузки моделей и без генерации: что где стоит, решает приложение.
# PRECISE_FP: проверки конечности и separating-axis тесты не должны
# зависеть от вольностей /fp:fast.
laiue_add_module(mesh_world
    PRECISE_FP
    RUNTIME_ROLE CORE
    SOURCES
        mesh_world.c
        mesh_world_service.h
        module_entry.c
        ../../api.h
    LINK
        laiue::math_support
)
