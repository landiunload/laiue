# Микшер переносим и не знает, куда выводит; системный вывод выбирается
# здесь. Offscreen-бэкенд собирается всегда: без него микс невозможно
# проверить на машине и в контейнере без звуковой подсистемы.
#
# У POSIX-вывода нет зависимости ни на сборке, ни на линковке: ALSA
# загружается в рантайме, поэтому модуль поднимается и там, где её нет.
if(LAIUE_PLATFORM_WINDOWS)
    set(audio_system_sources audio_wasapi.c)
    set(audio_system_links ole32 avrt)
elseif(LAIUE_PLATFORM_POSIX AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(audio_system_sources audio_alsa.c)
    set(audio_system_links)
else()
    # macOS и внешние профили своего вывода пока не имеют: запрос
    # системного бэкенда честно завершится ошибкой инициализации.
    set(audio_system_sources audio_system_unavailable.c)
    set(audio_system_links)
endif()

laiue_add_module(audio
    RUNTIME_ROLE GRAPHICS
    SOURCES
        audio_mixer.c
        audio_pack.c
        audio_pack.h
        audio_offscreen.c
        audio_offscreen.h
        audio_backend.h
        audio.h
        ../api.h
        ${audio_system_sources}
    PUBLIC_LINK
        laiue_content
    LINK
        laiue::math_support
        laiue::media_support
        ${audio_system_links}
)
