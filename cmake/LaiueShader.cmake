include_guard(GLOBAL)

function(laiue_find_shader_compiler out_variable)
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
    set(one_value_args SOURCE OUTPUT_DIRECTORY)
    cmake_parse_arguments(PARSE_ARGV 1 SHADER
        "${options}" "${one_value_args}" "")

    if(NOT SHADER_SOURCE OR NOT SHADER_OUTPUT_DIRECTORY)
        message(FATAL_ERROR
            "laiue_compile_shader требует SOURCE и OUTPUT_DIRECTORY")
    endif()

    laiue_find_shader_compiler(shader_compiler)
    if(NOT shader_compiler)
        message(STATUS
            "fxc.exe не найден — используются закоммиченные заголовки ${shader_name}")
        return()
    endif()

    file(MAKE_DIRECTORY "${SHADER_OUTPUT_DIRECTORY}")
    add_custom_command(
        OUTPUT
            "${SHADER_OUTPUT_DIRECTORY}/${shader_name}_vs.h"
            "${SHADER_OUTPUT_DIRECTORY}/${shader_name}_ps.h"
        COMMAND "${shader_compiler}"
            /nologo /T vs_5_0 /E VSMain /O3 /Qstrip_debug /Qstrip_reflect
            "/Fh${SHADER_OUTPUT_DIRECTORY}/${shader_name}_vs.h"
            /Vn "g_${shader_name}_vs" "${SHADER_SOURCE}"
        COMMAND "${shader_compiler}"
            /nologo /T ps_5_0 /E PSMain /O3 /Qstrip_debug /Qstrip_reflect
            "/Fh${SHADER_OUTPUT_DIRECTORY}/${shader_name}_ps.h"
            /Vn "g_${shader_name}_ps" "${SHADER_SOURCE}"
        DEPENDS "${SHADER_SOURCE}"
        VERBATIM
        COMMENT "Компиляция шейдера ${shader_name}"
    )
endfunction()
