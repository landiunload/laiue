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

if(LAIUE_RENDER_BACKEND_RESOLVED STREQUAL "VULKAN")
    find_package(Vulkan REQUIRED)
    set(backend_sources
        renderer_vulkan.c
        renderer_offscreen.h)
    set(backend_links Vulkan::Vulkan)
else()
    set(backend_sources
        renderer_d3d12.c
        ui_image_wic.c
        ui_image_wic.h)
    set(backend_links
        d3d12
        dxgi
        dxguid
        ole32
        windowscodecs)
endif()

laiue_add_module(render
    RUNTIME_ROLE GRAPHICS
    SOURCES
        ${backend_sources}
        renderer.h
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
    PUBLIC_LINK
        laiue_content
    LINK
        ${backend_links})

# Бэкенд включает render/generated/<backend>/*.h. Binary include идёт
# первым, поэтому обычная сборка никогда не перезаписывает checked-in
# fallback.
target_include_directories(laiue_render BEFORE PRIVATE
    "${PROJECT_BINARY_DIR}/src")
