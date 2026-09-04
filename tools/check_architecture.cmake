if(NOT DEFINED SOURCE_ROOT)
    get_filename_component(SOURCE_ROOT
        "${CMAKE_CURRENT_LIST_DIR}/../src" ABSOLUTE)
endif()

set(allowed_audio audio content math media platform)
set(allowed_content content platform)
set(allowed_input input)
set(allowed_math math)
set(allowed_media media)
set(allowed_mesh mesh platform render world)
set(allowed_mod mod platform)
set(allowed_physics physics)
set(allowed_platform platform)
set(allowed_render content media platform render)
set(allowed_runtime runtime)
set(allowed_scene math mesh platform render scene world)
set(allowed_ui math render scene ui)
set(allowed_world platform world)

# Переносимое ядро обязано оставаться переносимым. Правила выше смотрят
# только на "..."-включения между модулями, поэтому обращение прямо в
# системный API — HeapAlloc через <windows.h>, поток через <pthread.h> —
# они пропускают: угловые скобки для них невидимы. Ниже перечислены
# модули, которым разрешены лишь стандартные заголовки C и интринсики
# процессора; всё остальное они обязаны получать через platform/system.h.
#
# Модули, отсутствующие в списке, привязаны к платформе или бэкенду
# осознанно: platform — сама граница ОС, а render, ui, audio и input пока
# написаны на Win32/D3D12 либо Vulkan.
set(portable_modules content math media mesh mod physics runtime scene world)
set(portable_system_headers
    assert.h float.h inttypes.h iso646.h limits.h stdalign.h stdarg.h
    stdbool.h stddef.h stdint.h stdnoreturn.h string.h wchar.h
    arm64_neon.h arm64intr.h arm_neon.h emmintrin.h immintrin.h intrin.h
    nmmintrin.h pmmintrin.h smmintrin.h tmmintrin.h x86intrin.h
    xmmintrin.h)

file(GLOB_RECURSE source_files
    "${SOURCE_ROOT}/*.c" "${SOURCE_ROOT}/*.h")
set(violations)
set(checked 0)
foreach(source_file IN LISTS source_files)
    file(RELATIVE_PATH relative "${SOURCE_ROOT}" "${source_file}")
    string(REPLACE "\\" "/" relative "${relative}")
    if(relative MATCHES "(^|/)generated/" OR
       NOT relative MATCHES "^([^/]+)/")
        continue()
    endif()
    set(owner "${CMAKE_MATCH_1}")
    if(NOT DEFINED allowed_${owner})
        list(APPEND violations
            "${relative}: unknown engine module '${owner}'")
        continue()
    endif()
    math(EXPR checked "${checked} + 1")
    file(STRINGS "${source_file}" lines)
    set(line_number 0)
    foreach(line IN LISTS lines)
        math(EXPR line_number "${line_number} + 1")
        if(line MATCHES "^[ \t]*#[ \t]*include[ \t]*<([^>]+)>")
            set(system_include "${CMAKE_MATCH_1}")
            list(FIND portable_modules "${owner}" portable_index)
            if(NOT portable_index EQUAL -1)
                list(FIND portable_system_headers "${system_include}"
                    header_index)
                if(header_index EQUAL -1)
                    list(APPEND violations
                        "${relative}:${line_number}: portable module '${owner}' cannot include system header '${system_include}'")
                endif()
            endif()
            continue()
        endif()
        if(NOT line MATCHES
           "^[ \t]*#[ \t]*include[ \t]*\"([^\"]+)\"")
            continue()
        endif()
        set(include "${CMAKE_MATCH_1}")
        string(REPLACE "\\" "/" include "${include}")
        if(include MATCHES "(^|/)\\.\\.(/|$)")
            list(APPEND violations
                "${relative}:${line_number}: relative include '${include}' is forbidden")
            continue()
        endif()
        if(NOT include MATCHES "^([^/]+)/")
            continue()
        endif()
        set(dependency "${CMAKE_MATCH_1}")
        list(FIND allowed_${owner} "${dependency}" allowed_index)
        if(allowed_index EQUAL -1)
            list(APPEND violations
                "${relative}:${line_number}: module '${owner}' cannot include '${include}'")
        endif()
    endforeach()
endforeach()

if(violations)
    list(JOIN violations "\n  " formatted)
    message(FATAL_ERROR "Architecture boundary violations:\n  ${formatted}")
endif()
message(STATUS "Architecture boundaries: OK (${checked} files)")
