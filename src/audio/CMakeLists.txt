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

# Existing applications still call AudioPack* directly.  Keep that source
# surface in a deliberately non-runtime compatibility target, where the
# service indirection is backed by local adapters.  It is never added to
# laiue::engine and is not installed as an official provider, so the new
# module remains import-independent while old consumers have a migration
# path.
if(LAIUE_BUILD_AUDIO_PACK AND TARGET laiue_audio)
    add_library(laiue_audio_pack_legacy ${LAIUE_MODULE_LIBRARY_TYPE_RESOLVED})
    target_sources(laiue_audio_pack_legacy PRIVATE
        audio_pack.c
        audio_pack.h
        audio_pack_legacy_runtime.c
        audio_pack_runtime.h
        ../api.h)
    target_compile_definitions(laiue_audio_pack_legacy PRIVATE LAIUE_BUILD_AUDIO_PACK=1)
    if(LAIUE_MODULE_LIBRARY_TYPE_RESOLVED STREQUAL "STATIC")
        target_compile_definitions(laiue_audio_pack_legacy PUBLIC LAIUE_STATIC=1)
    elseif(NOT MSVC)
        set_target_properties(laiue_audio_pack_legacy PROPERTIES
            C_VISIBILITY_PRESET hidden
            VISIBILITY_INLINES_HIDDEN YES)
    endif()
    target_include_directories(laiue_audio_pack_legacy
        PUBLIC
            "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>"
            "$<INSTALL_INTERFACE:include/laiue>")
    target_link_libraries(laiue_audio_pack_legacy
        PRIVATE
            laiue_common
            laiue::platform_support
            laiue::math_support
            laiue::media_support
            laiue_audio
            laiue_content)
    if(LAIUE_MODULE_LIBRARY_TYPE_RESOLVED STREQUAL "SHARED")
        if(LAIUE_PLATFORM_WINDOWS)
            target_link_options(laiue_audio_pack_legacy PRIVATE /NOENTRY)
        elseif(APPLE)
            set_target_properties(laiue_audio_pack_legacy PROPERTIES
                BUILD_RPATH "@loader_path"
                INSTALL_RPATH "@loader_path"
                MACOSX_RPATH ON)
        else()
            set_target_properties(laiue_audio_pack_legacy PROPERTIES
                BUILD_RPATH "$ORIGIN"
                INSTALL_RPATH "$ORIGIN")
        endif()
    endif()
endif()
