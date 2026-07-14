set(generated_directory "${PROJECT_SOURCE_DIR}/src/render/generated")
laiue_compile_shader(chunk
    SOURCE "${PROJECT_SOURCE_DIR}/shaders/chunk.hlsl"
    OUTPUT_DIRECTORY "${generated_directory}"
)

laiue_add_module(render
    SOURCES
        renderer_d3d12.c
        renderer.h
        chunk_geometry.h
        ../api.h
        "${generated_directory}/chunk_vs.h"
        "${generated_directory}/chunk_ps.h"
    LINK
        d3d12
        dxgi
        dxguid
)
