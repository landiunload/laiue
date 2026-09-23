include_guard(GLOBAL)

function(laiue_assert_build_architecture)
    get_property(runtime_targets GLOBAL PROPERTY LAIUE_RUNTIME_TARGETS)
    get_target_property(engine_links
        laiue_engine INTERFACE_LINK_LIBRARIES)
    if(NOT "${engine_links}" STREQUAL "${runtime_targets}")
        message(FATAL_ERROR
            "laiue::engine must expose exactly the built runtime modules.\n"
            "Expected: ${runtime_targets}\n"
            "Actual:   ${engine_links}")
    endif()

    if(NOT LAIUE_BUILD_GRAPHICS)
        foreach(graphics_target IN ITEMS
                laiue_window laiue_input laiue_mesher laiue_render laiue_scene)
            if(TARGET "${graphics_target}")
                message(FATAL_ERROR
                    "Core-only graph unexpectedly contains ${graphics_target}")
            endif()
        endforeach()
    endif()
    if(NOT LAIUE_BUILD_AUDIO AND TARGET laiue_audio)
        message(FATAL_ERROR "Audio-disabled graph unexpectedly contains laiue_audio")
    endif()
    if(NOT LAIUE_BUILD_AUDIO_PACK AND TARGET laiue_audio_pack)
        message(FATAL_ERROR "Audio-pack-disabled graph unexpectedly contains laiue_audio_pack")
    endif()
    if(NOT LAIUE_BUILD_UI AND TARGET laiue_ui)
        message(FATAL_ERROR "UI-disabled graph unexpectedly contains laiue_ui")
    endif()

    message(STATUS
        "CMake architecture: engine libraries and platform split OK")
endfunction()
