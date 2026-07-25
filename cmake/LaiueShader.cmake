include_guard(GLOBAL)

function(laiue_find_shader_compiler out_variable)
    if(NOT WIN32)
        set(${out_variable} "" PARENT_SCOPE)
        return()
    endif()
    find_program(shader_compiler fxc)
    if(NOT shader_compiler)
        file(GLOB candidates
            "$ENV{ProgramFiles\(x86\)}/Windows Kits/10/bin/10.*/x64/fxc.exe")
        if(candidates)
            list(SORT candidates COMPARE NATURAL)
            list(GET candidates -1 shader_compiler)
        endif()
    endif()
    set(${out_variable} "${shader_compiler}" PARENT_SCOPE)
endfunction()

function(laiue_compile_shader shader_name)
    set(options)
    set(one_value_args
        SOURCE
        OUTPUT_DIRECTORY
        FALLBACK_DIRECTORY
        OUTPUT_VARIABLE)
    cmake_parse_arguments(PARSE_ARGV 1 SHADER
        "${options}" "${one_value_args}" "")

    if(NOT SHADER_SOURCE OR NOT SHADER_OUTPUT_DIRECTORY
       OR NOT SHADER_FALLBACK_DIRECTORY)
        message(FATAL_ERROR
            "laiue_compile_shader требует SOURCE, OUTPUT_DIRECTORY и "
            "FALLBACK_DIRECTORY")
    endif()

    set(vertex_output
        "${SHADER_OUTPUT_DIRECTORY}/${shader_name}_vs.h")
    set(pixel_output
        "${SHADER_OUTPUT_DIRECTORY}/${shader_name}_ps.h")
    set(vertex_fallback
        "${SHADER_FALLBACK_DIRECTORY}/${shader_name}_vs.h")
    set(pixel_fallback
        "${SHADER_FALLBACK_DIRECTORY}/${shader_name}_ps.h")

    laiue_find_shader_compiler(shader_compiler)
    if(shader_compiler)
        add_custom_command(
            OUTPUT "${vertex_output}" "${pixel_output}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory
                "${SHADER_OUTPUT_DIRECTORY}"
            COMMAND "${shader_compiler}"
                /nologo /T vs_5_0 /E VSMain /O3 /Qstrip_debug /Qstrip_reflect
                "/Fh${vertex_output}"
                /Vn "g_${shader_name}_vs" "${SHADER_SOURCE}"
            COMMAND "${shader_compiler}"
                /nologo /T ps_5_0 /E PSMain /O3 /Qstrip_debug /Qstrip_reflect
                "/Fh${pixel_output}"
                /Vn "g_${shader_name}_ps" "${SHADER_SOURCE}"
            DEPENDS "${SHADER_SOURCE}"
            VERBATIM
            COMMENT "Компиляция шейдера ${shader_name}")
    else()
        add_custom_command(
            OUTPUT "${vertex_output}" "${pixel_output}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory
                "${SHADER_OUTPUT_DIRECTORY}"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${vertex_fallback}" "${vertex_output}"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${pixel_fallback}" "${pixel_output}"
            DEPENDS "${vertex_fallback}" "${pixel_fallback}"
            VERBATIM
            COMMENT "Копирование fallback-шейдера ${shader_name}")
    endif()

    set_source_files_properties("${vertex_output}" "${pixel_output}"
        PROPERTIES GENERATED TRUE)
    add_custom_target("laiue_shader_${shader_name}"
        DEPENDS "${vertex_output}" "${pixel_output}")

    if(NOT TARGET laiue_verify_shaders)
        add_custom_target(laiue_verify_shaders)
    endif()
    add_custom_target("laiue_verify_shader_${shader_name}"
        COMMAND "${CMAKE_COMMAND}" -E compare_files
            "${vertex_output}" "${vertex_fallback}"
        COMMAND "${CMAKE_COMMAND}" -E compare_files
            "${pixel_output}" "${pixel_fallback}"
        DEPENDS "${vertex_output}" "${pixel_output}"
        COMMENT "Проверка checked-in shader fallback: ${shader_name}"
        VERBATIM)
    add_dependencies(laiue_verify_shaders
        "laiue_verify_shader_${shader_name}")

    if(SHADER_OUTPUT_VARIABLE)
        set(${SHADER_OUTPUT_VARIABLE}
            "${vertex_output}" "${pixel_output}"
            PARENT_SCOPE)
    endif()
endfunction()
