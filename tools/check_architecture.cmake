if(NOT DEFINED SOURCE_ROOT)
    get_filename_component(SOURCE_ROOT
        "${CMAKE_CURRENT_LIST_DIR}/../src" ABSOLUTE)
endif()

set(allowed_audio audio content math media mod platform)
set(allowed_character character mod platform)
set(allowed_content content mod platform)
set(allowed_graphics graphics)
set(allowed_input input mod)
set(allowed_math math)
set(allowed_media media)
set(allowed_mesh mesh mod platform render world)
set(allowed_mod mod platform)
set(allowed_numeric mod numeric platform)
# Физике доступны только числа: реализация мира ей по-прежнему не
# видна, свойства блоков приходят через callback.
set(allowed_physics math mod numeric physics task)
set(allowed_platform mod platform)
set(allowed_render content media mod platform render)
set(allowed_runtime runtime)
set(allowed_task mod task platform)
set(allowed_scene math mesh mod platform render scene world)
set(allowed_voxel_render math mesh mod platform render scene voxel_render world)
set(allowed_ui math mod platform render ui)
set(allowed_voxel mod platform voxel world)
set(allowed_world mod numeric platform world)

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
set(portable_modules character content graphics math media mesh mod numeric physics runtime scene task voxel voxel_render world)
set(portable_system_headers
    assert.h float.h inttypes.h iso646.h limits.h stdalign.h stdarg.h
    stdbool.h stddef.h stdint.h stdnoreturn.h string.h wchar.h
    arm64_neon.h arm64intr.h arm_neon.h emmintrin.h immintrin.h intrin.h
    nmmintrin.h pmmintrin.h smmintrin.h tmmintrin.h x86intrin.h
    xmmintrin.h)

# Physical source folders describe the technology family, while the second
# path component names the logical module whose include boundary is checked.
# Keeping that mapping here lets us organize the tree without weakening the
# existing module-level dependency rules or changing the public include names.
function(laiue_logical_owner relative_path output_variable)
    string(REPLACE "\\" "/" normalized_path "${relative_path}")
    if(NOT normalized_path MATCHES "^([^/]+)/")
        set(${output_variable} "" PARENT_SCOPE)
        return()
    endif()
    set(owner "${CMAKE_MATCH_1}")
    if(owner MATCHES "^(core|assets|graphics|jobs|simulation|modding)$")
        string(REGEX MATCH "^[^/]+/([^/]+)/" nested_module
            "${normalized_path}")
        if(nested_module)
            set(owner "${CMAKE_MATCH_1}")
        endif()
    endif()
    set(${output_variable} "${owner}" PARENT_SCOPE)
endfunction()

set(canonical_prefix_graphics "graphics")
set(canonical_prefix_audio "audio")
set(canonical_prefix_ui "ui")
set(canonical_prefix_platform "platform")
set(canonical_prefix_numeric "numeric")
set(canonical_prefix_math "core/math")
set(canonical_prefix_runtime "core/runtime")
set(canonical_prefix_media "assets/media")
set(canonical_prefix_content "assets/content")
set(canonical_prefix_input "graphics/input")
set(canonical_prefix_mesh "graphics/mesh")
set(canonical_prefix_render "graphics/render")
set(canonical_prefix_scene "graphics/scene")
set(canonical_prefix_voxel_render "graphics/voxel_render")
set(canonical_prefix_task "jobs/task")
set(canonical_prefix_world "simulation/world")
set(canonical_prefix_physics "simulation/physics")
set(canonical_prefix_character "simulation/character")
set(canonical_prefix_voxel "simulation/voxel")
set(canonical_prefix_mod "modding/mod")

file(GLOB_RECURSE source_files
    "${SOURCE_ROOT}/*.c" "${SOURCE_ROOT}/*.h")
set(violations)
set(checked 0)
foreach(source_file IN LISTS source_files)
    file(RELATIVE_PATH relative "${SOURCE_ROOT}" "${source_file}")
    string(REPLACE "\\" "/" relative "${relative}")
    if(relative MATCHES "(^|/)generated/")
        continue()
    endif()
    laiue_logical_owner("${relative}" owner)
    if(owner STREQUAL "")
        continue()
    endif()
    set(expected_prefix "${canonical_prefix_${owner}}")
    if(NOT expected_prefix STREQUAL "" AND
       NOT relative MATCHES "^${expected_prefix}(/|$)")
        list(APPEND violations
            "${relative}: module '${owner}' must live under '${expected_prefix}'")
        continue()
    endif()
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
