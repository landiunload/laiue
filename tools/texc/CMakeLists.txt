# Офлайн-инструмент подготовки содержимого. В SDK он не входит и
# необязателен: движок читает и PNG с GIF и JPEG, и свой `.lt`, и сам
# готовит второй из первых. Инструмент нужен там, где содержимое
# собирают заранее — например когда каталог пака доступен только для
# чтения.
#
# Своего кода форматов у него нет: и декодеры, и запись `.lt` лежат в
# общей `media_support`, которой пользуется движок. Отдельная
# статическая библиотека-ядро поэтому больше не нужна — тест проверяет
# ту же самую реализацию напрямую.
add_executable(laiue_texc main.c)
target_link_libraries(laiue_texc PRIVATE laiue::media_support)
laiue_configure_standalone_executable(laiue_texc TexcEntryPoint)
if(WIN32)
    target_link_libraries(laiue_texc PRIVATE shell32)
endif()
