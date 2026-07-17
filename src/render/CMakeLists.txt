set(generated_directory "${PROJECT_SOURCE_DIR}/src/render/generated")
laiue_compile_shader(chunk
    SOURCE "${PROJECT_SOURCE_DIR}/shaders/chunk.hlsl"
    OUTPUT_DIRECTORY "${generated_directory}"
)
laiue_compile_shader(panorama
    SOURCE "${PROJECT_SOURCE_DIR}/shaders/panorama.hlsl"
    OUTPUT_DIRECTORY "${generated_directory}"
)
laiue_compile_shader(ui
    SOURCE "${PROJECT_SOURCE_DIR}/shaders/ui.hlsl"
    OUTPUT_DIRECTORY "${generated_directory}"
)

laiue_add_module(render
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
        "${generated_directory}/chunk_vs.h"
        "${generated_directory}/chunk_ps.h"
        "${generated_directory}/panorama_vs.h"
        "${generated_directory}/panorama_ps.h"
        "${generated_directory}/ui_vs.h"
        "${generated_directory}/ui_ps.h"
    LINK
        laiue_content
        d3d12
        dxgi
        dxguid
        ole32
        windowscodecs
)
