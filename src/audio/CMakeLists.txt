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

if(LAIUE_BUILD_AUDIO)
    laiue_add_module(audio
        RUNTIME_ROLE CORE
        SOURCES
            audio_mixer.c
            audio_offscreen.c
            audio_offscreen.h
            audio_backend.h
            audio.h
            audio_service.h
            module_entry.c
            ../api.h
            ${audio_system_sources}
        LINK
            laiue::math_support
            ${audio_system_links}
    )
endif()

# Pack discovery and decoding is a separate technology.  It is allowed to
# disappear while the mixer and its offscreen/system output remain usable.
# The module entry receives both dependencies through the bootstrap service
# tables; no LAIUE DLL is imported by this target.
if(LAIUE_BUILD_AUDIO_PACK)
    laiue_add_module(audio_pack
        RUNTIME_ROLE CORE
        SOURCES
            audio_pack.c
            audio_pack.h
            audio_pack_runtime.c
            audio_pack_runtime.h
            audio_pack_service.h
            pack_module_entry.c
            ../api.h
        LINK
            laiue::math_support
            laiue::media_support
    )
endif()
