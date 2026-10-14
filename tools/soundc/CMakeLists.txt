# Офлайн-инструмент подготовки содержимого. В SDK он не входит и
# необязателен: движок читает и WAV с MP3, и свой `.la`, и сам готовит
# второй из первых. Инструмент нужен там, где содержимое собирают
# заранее, и там, где нужен ADPCM: движок сам сжимать не станет, потому
# что это потеря качества, а не решение загрузчика.
#
# Своего кода форматов у него нет: и декодеры, и запись `.la` лежат в
# общей `media_support`, которой пользуется движок.
add_executable(laiue_soundc main.c)
target_link_libraries(laiue_soundc PRIVATE laiue::media_support)
laiue_configure_standalone_executable(laiue_soundc SoundcEntryPoint)
if(WIN32)
    # Разбор кавычек командной строки берётся у системы: собственный
    # парсер отличался бы от неё ровно в тех путях, которые пользователь
    # и заключает в кавычки.
    target_link_libraries(laiue_soundc PRIVATE shell32)
endif()
