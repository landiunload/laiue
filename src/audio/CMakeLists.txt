# The mixer does not know where frames are sent.  Platform output is selected
# by the separate `audio_output` provider below.  The offscreen backend is
# always part of the mixer so it remains testable without a sound device.
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

# The mixer is self-contained: it always ships the deterministic offscreen
# backend and resolves system output through the optional provider service.
if(LAIUE_BUILD_AUDIO)
    laiue_add_module(audio
        RUNTIME_ROLE CORE
        SOURCES
            audio_mixer.c
            audio_mixer_internal.h
            audio_offscreen.c
            audio_offscreen.h
            audio_backend.h
            audio.h
            audio_service.h
            audio_output_service.h
            module_entry.c
            ../api.h
        LINK
            laiue::math_support
    )
endif()

# Platform output is a provider in its own binary. Removing this module now
# leaves the mixer and offscreen tests usable; only AUDIO_BACKEND_SYSTEM is
# unavailable at runtime.
if(LAIUE_BUILD_AUDIO_OUTPUT)
    laiue_add_module(audio_output
        RUNTIME_ROLE CORE
        SOURCES
            audio_backend.h
            audio_output_service.h
            output_module_entry.c
            ../api.h
            ${audio_system_sources}
        LINK
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
