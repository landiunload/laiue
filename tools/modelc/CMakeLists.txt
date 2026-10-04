# Офлайн-инструмент подготовки моделей. В SDK он не входит и необязателен:
# движок читает OBJ из пака и сам готовит рядом `.obj.lo`. Своего кода
# формата у него нет — разбор и запись лежат в общей `media_support`.
add_executable(laiue_modelc main.c)
target_link_libraries(laiue_modelc PRIVATE laiue::media_support)
laiue_configure_standalone_executable(laiue_modelc ModelcEntryPoint)
if(WIN32)
    target_link_libraries(laiue_modelc PRIVATE shell32)
endif()
