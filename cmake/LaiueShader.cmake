include_guard(GLOBAL)

# Шейдеры пишутся один раз на HLSL и компилируются под активный бэкенд:
# fxc даёт DXBC для D3D12, glslang — SPIR-V для Vulkan. Оба результата
# хранятся в дереве как checked-in fallback, поэтому сборка без
# установленного компилятора шейдеров возможна на любой платформе.
#
# Раскладка дескрипторов SPIR-V задаётся сдвигами регистров, а не правкой
# HLSL: fxc не понимает [[vk::binding]], а общий исходник важнее. Сдвиги
# переводят HLSL-регистры в один descriptor set без коллизий:
#
#   b0        -> binding 0   (uniform buffer)
#   t0..t3    -> binding 1..4 (storage buffer или sampled image)
#   s0        -> binding 5   (sampler)
#
# ByteAddressBuffer относится к классу SSBO, а не текстур, поэтому ему
# нужен отдельный сдвиг --sbb с тем же значением, что и --stb: оба класса
# нумеруются в общем пространстве t-регистров.
set(LAIUE_SPIRV_BINDING_SHIFT_TEXTURE 1)
set(LAIUE_SPIRV_BINDING_SHIFT_SAMPLER 5)

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

function(laiue_find_spirv_compiler out_variable)
    find_program(spirv_compiler NAMES glslangValidator glslang)
    set(${out_variable} "${spirv_compiler}" PARENT_SCOPE)
endfunction()

# Общая часть обоих бэкендов: объявляет правило генерации пары заголовков,
# fallback-копирование, цель проверки и возврат списка выходных файлов.
function(_laiue_register_shader_outputs shader_name backend_directory
         vertex_output pixel_output vertex_fallback pixel_fallback
         out_variable)
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

    set(${out_variable} "${vertex_output}" "${pixel_output}" PARENT_SCOPE)
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

    string(TOLOWER "${LAIUE_RENDER_BACKEND_RESOLVED}" backend_directory)
    set(output_directory "${SHADER_OUTPUT_DIRECTORY}/${backend_directory}")
    set(fallback_directory "${SHADER_FALLBACK_DIRECTORY}/${backend_directory}")
    set(vertex_output "${output_directory}/${shader_name}_vs.h")
    set(pixel_output "${output_directory}/${shader_name}_ps.h")
    set(vertex_fallback "${fallback_directory}/${shader_name}_vs.h")
    set(pixel_fallback "${fallback_directory}/${shader_name}_ps.h")

    if(LAIUE_RENDER_BACKEND_RESOLVED STREQUAL "VULKAN")
        laiue_find_spirv_compiler(shader_compiler)
        set(compile_vertex
            COMMAND "${shader_compiler}"
                -D --target-env vulkan1.1
                --sub 0
                --stb "${LAIUE_SPIRV_BINDING_SHIFT_TEXTURE}"
                --sbb "${LAIUE_SPIRV_BINDING_SHIFT_TEXTURE}"
                --suavb "${LAIUE_SPIRV_BINDING_SHIFT_TEXTURE}"
                --ssb "${LAIUE_SPIRV_BINDING_SHIFT_SAMPLER}"
                -e VSMain -S vert
                --vn "g_${shader_name}_vs"
                -o "${vertex_output}" "${SHADER_SOURCE}")
        set(compile_pixel
            COMMAND "${shader_compiler}"
                -D --target-env vulkan1.1
                --sub 0
                --stb "${LAIUE_SPIRV_BINDING_SHIFT_TEXTURE}"
                --sbb "${LAIUE_SPIRV_BINDING_SHIFT_TEXTURE}"
                --suavb "${LAIUE_SPIRV_BINDING_SHIFT_TEXTURE}"
                --ssb "${LAIUE_SPIRV_BINDING_SHIFT_SAMPLER}"
                -e PSMain -S frag
                --vn "g_${shader_name}_ps"
                -o "${pixel_output}" "${SHADER_SOURCE}")
    else()
        laiue_find_shader_compiler(shader_compiler)
        set(compile_vertex
            COMMAND "${shader_compiler}"
                /nologo /T vs_5_0 /E VSMain /O3 /Qstrip_debug /Qstrip_reflect
                "/Fh${vertex_output}"
                /Vn "g_${shader_name}_vs" "${SHADER_SOURCE}")
        set(compile_pixel
            COMMAND "${shader_compiler}"
                /nologo /T ps_5_0 /E PSMain /O3 /Qstrip_debug /Qstrip_reflect
                "/Fh${pixel_output}"
                /Vn "g_${shader_name}_ps" "${SHADER_SOURCE}")
    endif()

    if(shader_compiler)
        add_custom_command(
            OUTPUT "${vertex_output}" "${pixel_output}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${output_directory}"
            ${compile_vertex}
            ${compile_pixel}
            COMMAND "${CMAKE_COMMAND}"
                "-DINPUT_FILE=${vertex_output}"
                -P "${PROJECT_SOURCE_DIR}/cmake/NormalizeShaderHeader.cmake"
            COMMAND "${CMAKE_COMMAND}"
                "-DINPUT_FILE=${pixel_output}"
                -P "${PROJECT_SOURCE_DIR}/cmake/NormalizeShaderHeader.cmake"
            DEPENDS "${SHADER_SOURCE}"
            VERBATIM
            COMMENT "Компиляция шейдера ${shader_name}")
    else()
        add_custom_command(
            OUTPUT "${vertex_output}" "${pixel_output}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${output_directory}"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${vertex_fallback}" "${vertex_output}"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${pixel_fallback}" "${pixel_output}"
            DEPENDS "${vertex_fallback}" "${pixel_fallback}"
            VERBATIM
            COMMENT "Копирование fallback-шейдера ${shader_name}")
    endif()

    _laiue_register_shader_outputs("${shader_name}" "${backend_directory}"
        "${vertex_output}" "${pixel_output}"
        "${vertex_fallback}" "${pixel_fallback}"
        shader_outputs)

    if(SHADER_OUTPUT_VARIABLE)
        set(${SHADER_OUTPUT_VARIABLE} ${shader_outputs} PARENT_SCOPE)
    endif()
endfunction()
