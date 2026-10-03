# Пак моделей — самостоятельная технология: геометрия без GPU, мира и
# материалов. Каталог содержимого подключается необязательным сервисом,
# поэтому модуль работает и в профиле без него.
laiue_add_module(model
    RUNTIME_ROLE CORE
    SOURCES
        model_pack.c
        model_service.h
        module_entry.c
        ../../api.h
    LINK
        laiue::media_support
)
