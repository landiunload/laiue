include_guard(GLOBAL)

# Шейдеры пишутся один раз на HLSL, и один компилятор — DXC — собирает их
# под каждый слинкованный бэкенд: DXIL (Shader Model 6.0) для D3D12 и
# SPIR-V для Vulkan. Оба результата хранятся в дереве как checked-in
# fallback, поэтому сборка без DXC возможна на любой платформе.
#
# Shader Model 6.0 и SPIR-V для Vulkan 1.1 — нижние границы, которые
# держат все поддерживаемые устройства; встроенным шейдерам больше не
# нужно. Это же — контракт шейдерпаков (docs/shaderpacks.md).
set(LAIUE_SHADER_MODEL 6_0)
set(LAIUE_SPIRV_TARGET_ENV vulkan1.1)

# Раскладка дескрипторов SPIR-V задаётся сдвигами регистров, а не
# атрибутами [[vk::binding]]: общий исходник остаётся без Vulkan-разметки.
# Сдвиги переводят HLSL-регистры в один descriptor set без коллизий:
#
#   b0        -> binding 0   (uniform buffer)
#   t0..t3    -> binding 1..4 (storage buffer или sampled image)
#   s0        -> binding 5   (sampler)
set(LAIUE_SPIRV_BINDING_SHIFT_TEXTURE 1)
set(LAIUE_SPIRV_BINDING_SHIFT_SAMPLER 5)

option(LAIUE_REQUIRE_SHADER_COMPILER
    "Fail configuration instead of using checked-in shaders when DXC is unusable" OFF)

set(LAIUE_DXC "" CACHE FILEPATH
    "DXC for the built-in shaders; empty searches PATH, the Vulkan SDK and the Windows SDK")

# Кандидаты по порядку: явный LAIUE_DXC, иначе dxc из PATH, Vulkan SDK и
# Windows SDK от новых к старым.
function(_laiue_dxc_candidates out_variable)
    if(LAIUE_DXC)
        set(${out_variable} "${LAIUE_DXC}" PARENT_SCOPE)
        return()
    endif()
    set(directories)
    if(DEFINED ENV{VULKAN_SDK})
        list(APPEND directories "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/bin")
    endif()
    if(CMAKE_HOST_WIN32)
        set(kit_architecture x64)
        if(CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "^(ARM64|arm64|aarch64)$")
            set(kit_architecture arm64)
        endif()
        file(GLOB kits "$ENV{ProgramFiles\(x86\)}/Windows Kits/10/bin/10.*")
        list(SORT kits COMPARE NATURAL ORDER DESCENDING)
        foreach(kit IN LISTS kits)
            list(APPEND directories "${kit}/${kit_architecture}")
        endforeach()
    endif()

    set(candidates)
    unset(compiler)
    find_program(compiler NAMES dxc NO_CACHE)
    if(compiler)
        list(APPEND candidates "${compiler}")
    endif()
    foreach(directory IN LISTS directories)
        unset(compiler)
        find_program(compiler NAMES dxc PATHS "${directory}" NO_DEFAULT_PATH NO_CACHE)
        if(compiler)
            list(APPEND candidates "${compiler}")
        endif()
    endforeach()
    list(REMOVE_DUPLICATES candidates)
    set(${out_variable} ${candidates} PARENT_SCOPE)
endfunction()

# Сборки DXC умеют разное: в DXC из Windows SDK нет SPIR-V, а DXC без
# валидатора оставляет DXIL неподписанным, и такой байткод D3D12 не
# принимает. Поэтому кандидат проверяется пробной компиляцией: SPIR-V для
# Vulkan, DXIL с ненулевым хешем контейнера для D3D12.
function(_laiue_dxc_probe compiler backend out_variable)
    set(directory "${CMAKE_BINARY_DIR}/CMakeFiles/laiue_dxc_probe")
    set(source "${directory}/probe.hlsl")
    set(output "${directory}/probe_${backend}.bin")
    file(WRITE "${source}" "float4 main() : SV_Position { return 0; }\n")
    file(REMOVE "${output}")
    set(options)
    if(backend STREQUAL "VULKAN")
        set(options -spirv)
    endif()
    execute_process(
        COMMAND "${compiler}" ${options} -T "vs_${LAIUE_SHADER_MODEL}" -E main
            -Fo "${output}" "${source}"
        RESULT_VARIABLE result
        OUTPUT_QUIET ERROR_QUIET)
    set(usable FALSE)
    if(result EQUAL 0 AND EXISTS "${output}")
        set(usable TRUE)
        if(backend STREQUAL "D3D12")
            file(READ "${output}" header LIMIT 20 HEX)
            string(SUBSTRING "${header}" 8 32 digest)
            if(digest MATCHES "^0+$")
                set(usable FALSE)
            endif()
        endif()
    endif()
    set(${out_variable} ${usable} PARENT_SCOPE)
endfunction()

# Возвращает компилятор для бэкенда или пустую строку. Решение и сообщение
# о нём принимаются один раз на бэкенд, а не на каждый шейдер.
function(_laiue_shader_compiler backend out_variable)
    get_property(resolved GLOBAL PROPERTY "LAIUE_SHADER_COMPILER_${backend}" SET)
    if(resolved)
        get_property(compiler GLOBAL PROPERTY "LAIUE_SHADER_COMPILER_${backend}")
        set(${out_variable} "${compiler}" PARENT_SCOPE)
        return()
    endif()

    _laiue_dxc_candidates(candidates)
    set(compiler "")
    foreach(candidate IN LISTS candidates)
        _laiue_dxc_probe("${candidate}" "${backend}" usable)
        if(usable)
            set(compiler "${candidate}")
            break()
        endif()
    endforeach()
    if(NOT candidates)
        set(reason "DXC not found")
    else()
        list(JOIN candidates ", " tried)
        set(reason "no DXC builds ${backend} shaders (tried ${tried})")
    endif()

    if(compiler)
        message(STATUS "laiue ${backend} shaders: ${compiler}")
    elseif(LAIUE_REQUIRE_SHADER_COMPILER)
        message(FATAL_ERROR "laiue ${backend} shaders: ${reason}")
    else()
        message(STATUS "laiue ${backend} shaders: ${reason}, using checked-in bytecode")
    endif()
    set_property(GLOBAL PROPERTY "LAIUE_SHADER_COMPILER_${backend}" "${compiler}")
    set(${out_variable} "${compiler}" PARENT_SCOPE)
endfunction()

# Компилирует VSMain и PSMain одного HLSL-файла под бэкенд в заголовки
# <name>_vs.h и <name>_ps.h, а без DXC копирует checked-in fallback.
# Цель laiue_verify_shaders сравнивает результат с fallback байт в байт.
function(laiue_compile_shader shader_name)
    set(one_value_args
        BACKEND
        SOURCE
        OUTPUT_DIRECTORY
        FALLBACK_DIRECTORY
        OUTPUT_VARIABLE)
    cmake_parse_arguments(PARSE_ARGV 1 SHADER "" "${one_value_args}" "")

    if(NOT SHADER_BACKEND MATCHES "^(D3D12|VULKAN)$" OR NOT SHADER_SOURCE OR
       NOT SHADER_OUTPUT_DIRECTORY OR NOT SHADER_FALLBACK_DIRECTORY)
        message(FATAL_ERROR
            "laiue_compile_shader требует BACKEND (D3D12 или VULKAN), SOURCE, "
            "OUTPUT_DIRECTORY и FALLBACK_DIRECTORY")
    endif()

    string(TOLOWER "${SHADER_BACKEND}" backend_directory)
    set(output_directory "${SHADER_OUTPUT_DIRECTORY}/${backend_directory}")
    set(fallback_directory "${SHADER_FALLBACK_DIRECTORY}/${backend_directory}")
    set(headers)
    set(fallbacks)
    set(commands)

    _laiue_shader_compiler("${SHADER_BACKEND}" compiler)
    if(SHADER_BACKEND STREQUAL "VULKAN")
        set(target_options
            -spirv "-fspv-target-env=${LAIUE_SPIRV_TARGET_ENV}"
            -fvk-b-shift 0 0
            -fvk-t-shift "${LAIUE_SPIRV_BINDING_SHIFT_TEXTURE}" 0
            -fvk-s-shift "${LAIUE_SPIRV_BINDING_SHIFT_SAMPLER}" 0)
    else()
        set(target_options -Qstrip_debug -Qstrip_reflect)
    endif()

    foreach(stage IN ITEMS vs ps)
        if(stage STREQUAL "vs")
            set(entry VSMain)
        else()
            set(entry PSMain)
        endif()
        set(header "${output_directory}/${shader_name}_${stage}.h")
        set(fallback "${fallback_directory}/${shader_name}_${stage}.h")
        set(bytecode "${output_directory}/${shader_name}_${stage}.bin")
        list(APPEND headers "${header}")
        list(APPEND fallbacks "${fallback}")
        if(compiler)
            list(APPEND commands
                COMMAND "${compiler}" -nologo -HV 2021 -WX -O3 ${target_options}
                    -T "${stage}_${LAIUE_SHADER_MODEL}" -E "${entry}"
                    -Fo "${bytecode}" "${SHADER_SOURCE}"
                COMMAND "${CMAKE_COMMAND}"
                    "-DINPUT_FILE=${bytecode}" "-DOUTPUT_FILE=${header}"
                    "-DSYMBOL=g_${shader_name}_${stage}"
                    -P "${PROJECT_SOURCE_DIR}/cmake/EmbedShaderBytecode.cmake")
        else()
            list(APPEND commands
                COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${fallback}" "${header}")
        endif()
    endforeach()

    if(compiler)
        set(depends "${SHADER_SOURCE}" "${PROJECT_SOURCE_DIR}/cmake/EmbedShaderBytecode.cmake")
        set(comment "Компиляция шейдера ${shader_name} (${SHADER_BACKEND})")
    else()
        set(depends ${fallbacks})
        set(comment "Копирование fallback-шейдера ${shader_name} (${SHADER_BACKEND})")
    endif()
    add_custom_command(
        OUTPUT ${headers}
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${output_directory}"
        ${commands}
        DEPENDS ${depends}
        VERBATIM
        COMMENT "${comment}")
    set_source_files_properties(${headers} PROPERTIES GENERATED TRUE)

    if(NOT TARGET laiue_verify_shaders)
        add_custom_target(laiue_verify_shaders)
    endif()
    set(verify_target "laiue_verify_shader_${backend_directory}_${shader_name}")
    set(compare_commands)
    foreach(header fallback IN ZIP_LISTS headers fallbacks)
        list(APPEND compare_commands
            COMMAND "${CMAKE_COMMAND}" -E compare_files "${header}" "${fallback}")
    endforeach()
    add_custom_target("${verify_target}"
        ${compare_commands}
        DEPENDS ${headers}
        COMMENT "Проверка checked-in шейдера ${shader_name} (${SHADER_BACKEND})"
        VERBATIM)
    add_dependencies(laiue_verify_shaders "${verify_target}")

    if(SHADER_OUTPUT_VARIABLE)
        set(${SHADER_OUTPUT_VARIABLE} ${headers} PARENT_SCOPE)
    endif()
endfunction()
