include_guard(GLOBAL)

function(laiue_assert_target_excludes root_target)
    set(forbidden_targets ${ARGN})
    set(queue "${root_target}")
    set(visited)
    while(queue)
        list(POP_FRONT queue current)
        get_target_property(aliased_target "${current}" ALIASED_TARGET)
        if(aliased_target)
            set(current "${aliased_target}")
        endif()
        if(current IN_LIST visited)
            continue()
        endif()
        list(APPEND visited "${current}")

        foreach(forbidden_target IN LISTS forbidden_targets)
            if(current STREQUAL forbidden_target)
                message(FATAL_ERROR
                    "${root_target} транзитивно зависит от client-only "
                    "цели ${forbidden_target}")
            endif()
        endforeach()

        get_target_property(direct_links "${current}" LINK_LIBRARIES)
        get_target_property(interface_links
            "${current}" INTERFACE_LINK_LIBRARIES)
        foreach(dependency IN LISTS direct_links interface_links)
            if(TARGET "${dependency}")
                list(APPEND queue "${dependency}")
            endif()
        endforeach()
    endwhile()
endfunction()

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

        laiue_assert_target_excludes(laiue_server
            laiue_window laiue_input laiue_audio laiue_mesher
            laiue_render laiue_core)
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
