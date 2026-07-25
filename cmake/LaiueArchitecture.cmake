include_guard(GLOBAL)

function(laiue_assert_build_architecture)
    get_target_property(headless_links
        laiue_headless_stack INTERFACE_LINK_LIBRARIES)
    if(NOT "${headless_links}" STREQUAL "${LAIUE_HEADLESS_TARGETS}")
        message(FATAL_ERROR
            "laiue::headless_stack должен содержать ровно общие модули.\n"
            "Ожидалось: ${LAIUE_HEADLESS_TARGETS}\n"
            "Получено:  ${headless_links}")
    endif()

    if(TARGET laiue_core)
        get_target_property(client_links laiue_core LINK_LIBRARIES)
        list(FIND client_links "laiue::headless_stack" client_stack_index)
        if(client_stack_index EQUAL -1)
            message(FATAL_ERROR
                "Windows client composition root не использует "
                "laiue::headless_stack")
        endif()
    endif()

    if(TARGET laiue_server)
        get_target_property(server_links laiue_server LINK_LIBRARIES)
        list(FIND server_links "laiue::headless_stack" server_stack_index)
        if(server_stack_index EQUAL -1)
            message(FATAL_ERROR
                "Dedicated server не использует laiue::headless_stack")
        endif()

        foreach(forbidden_target IN ITEMS
                laiue_window laiue_input laiue_audio laiue_mesher
                laiue_render laiue_core)
            list(FIND server_links "${forbidden_target}" forbidden_index)
            if(NOT forbidden_index EQUAL -1)
                message(FATAL_ERROR
                    "Dedicated server напрямую зависит от client-only цели "
                    "${forbidden_target}")
            endif()
        endforeach()
    endif()

    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        foreach(client_target IN ITEMS
                laiue laiue_window laiue_input laiue_audio laiue_mesher
                laiue_render laiue_core)
            if(TARGET "${client_target}")
                message(FATAL_ERROR
                    "Linux server-only graph содержит client-only цель "
                    "${client_target}")
            endif()
        endforeach()
    endif()

    message(STATUS
        "CMake architecture: shared headless stack and platform split OK")
endfunction()
