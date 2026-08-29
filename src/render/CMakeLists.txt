set(generated_directory
    "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(fallback_directory
    "${PROJECT_SOURCE_DIR}/src/render/generated")

laiue_compile_shader(chunk
    SOURCE "${PROJECT_SOURCE_DIR}/shaders/chunk.hlsl"
    OUTPUT_DIRECTORY "${generated_directory}"
    FALLBACK_DIRECTORY "${fallback_directory}"
    OUTPUT_VARIABLE chunk_shader_headers)
laiue_compile_shader(panorama
    SOURCE "${PROJECT_SOURCE_DIR}/shaders/panorama.hlsl"
    OUTPUT_DIRECTORY "${generated_directory}"
    FALLBACK_DIRECTORY "${fallback_directory}"
    OUTPUT_VARIABLE panorama_shader_headers)
laiue_compile_shader(ui
    SOURCE "${PROJECT_SOURCE_DIR}/shaders/ui.hlsl"
    OUTPUT_DIRECTORY "${generated_directory}"
    FALLBACK_DIRECTORY "${fallback_directory}"
    OUTPUT_VARIABLE ui_shader_headers)

laiue_add_module(render
    RUNTIME_ROLE GRAPHICS
    SOURCES
        renderer_d3d12.c
        renderer.h
        ui_image_wic.c
        ui_image_wic.h
        shader_pack.c
        shader_pack.h
        texture_pack.c
        texture_pack.h
        texture_pack_internal.h
        chunk_geometry.h
        ../api.h
        ${chunk_shader_headers}
        ${panorama_shader_headers}
        ${ui_shader_headers}
    LINK
        laiue_content
        d3d12
        dxgi
        dxguid
        ole32
        windowscodecs)

# renderer_d3d12.c включает render/generated/*.h. Binary include идёт первым,
# поэтому обычная сборка никогда не перезаписывает checked-in fallback.
target_include_directories(laiue_render BEFORE PRIVATE
    "${PROJECT_BINARY_DIR}/src")
