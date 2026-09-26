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

# Физически доступный на этой платформе набор бэкендов. Оба могут быть
# слинкованы в один бинарник; выбор бэкенда при создании делает
# renderer_dispatch.c.
find_package(Vulkan QUIET)

set(available_sources "")
set(available_links "")
set(available_definitions "")
if(WIN32)
    list(APPEND available_sources renderer_d3d12.c ui_image_wic.c ui_image_wic.h)
    list(APPEND available_links d3d12 dxgi dxguid ole32 windowscodecs)
    list(APPEND available_definitions LAIUE_RENDER_HAS_D3D12)
endif()
if(Vulkan_FOUND)
    list(APPEND available_sources renderer_vulkan.c renderer_offscreen.h)
    list(APPEND available_links Vulkan::Vulkan)
    list(APPEND available_definitions LAIUE_RENDER_HAS_VULKAN)
endif()

# Явный выбор LAIUE_RENDER_BACKEND уважается: тогда линкуется ровно один
# бэкенд, как и раньше. Только AUTO (умолчание) линкует всё доступное
# сразу. Исходная опция сравнивается до resolve: на Windows AUTO тоже
# разрешается в D3D12, и по LAIUE_RENDER_BACKEND_RESOLVED их не отличить.
string(TOUPPER "${LAIUE_RENDER_BACKEND}" laiue_render_backend_requested)
set(LAIUE_RENDER_HAS_D3D12 OFF)
set(LAIUE_RENDER_HAS_VULKAN_TARGET OFF)
if(laiue_render_backend_requested STREQUAL "AUTO")
    set(backend_sources renderer_dispatch.c ${available_sources})
    set(backend_links ${available_links})
    set(backend_compile_definitions ${available_definitions})
    if(WIN32)
        set(LAIUE_RENDER_HAS_D3D12 ON)
    endif()
    if(Vulkan_FOUND)
        set(LAIUE_RENDER_HAS_VULKAN_TARGET ON)
    endif()
elseif(laiue_render_backend_requested STREQUAL "D3D12")
    set(backend_sources
        renderer_dispatch.c
        renderer_d3d12.c
        ui_image_wic.c
        ui_image_wic.h)
    set(backend_links
        d3d12
        dxgi
        dxguid
        ole32
        windowscodecs)
    set(backend_compile_definitions LAIUE_RENDER_HAS_D3D12)
    set(LAIUE_RENDER_HAS_D3D12 ON)
elseif(laiue_render_backend_requested STREQUAL "VULKAN")
    if(Vulkan_FOUND)
        set(backend_sources renderer_dispatch.c renderer_vulkan.c renderer_offscreen.h)
        set(backend_links Vulkan::Vulkan)
        set(backend_compile_definitions LAIUE_RENDER_HAS_VULKAN)
        set(LAIUE_RENDER_HAS_VULKAN_TARGET ON)
    else()
        set(backend_sources renderer_dispatch.c)
        set(backend_links "")
        set(backend_compile_definitions "")
    endif()
else()
    message(FATAL_ERROR
        "LAIUE_RENDER_BACKEND must be AUTO, D3D12 or VULKAN")
endif()

if(NOT backend_compile_definitions)
    message(FATAL_ERROR
        "No render backend available: need Windows (D3D12) or a Vulkan SDK.")
endif()

# RendererCreate()/RENDERER_BACKEND_AUTO выбирает тот бэкенд, который был
# разрешён из опции сборки: на Windows это D3D12 (сегодняшнее поведение),
# иначе Vulkan. При явном VULKAN на Windows сюда тоже попадёт Vulkan.
if(LAIUE_RENDER_BACKEND_RESOLVED STREQUAL "D3D12")
    list(APPEND backend_compile_definitions LAIUE_RENDER_DEFAULT_BACKEND_D3D12)
else()
    list(APPEND backend_compile_definitions LAIUE_RENDER_DEFAULT_BACKEND_VULKAN)
endif()

laiue_add_module(render
    RUNTIME_ROLE GRAPHICS
    SOURCES
        ${backend_sources}
        renderer.h
        renderer_internal.h
        shader_pack.c
        shader_pack.h
        texture_build.c
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
        ${backend_links}
        laiue::media_support)

target_compile_definitions(laiue_render PRIVATE ${backend_compile_definitions})

# Бэкенд включает render/generated/<backend>/*.h. Binary include идёт
# первым, поэтому обычная сборка никогда не перезаписывает checked-in
# fallback.
target_include_directories(laiue_render BEFORE PRIVATE
    "${PROJECT_BINARY_DIR}/src")

# Тесты живут в каталоге, который добавляется после src/render, и решают,
# какие прогоны регистрировать, по реально слинкованным бэкендам, а не по
# LAIUE_RENDER_BACKEND_RESOLVED: на Windows AUTO разрешается в D3D12 и по
# resolved неотличим от явного D3D12, хотя линкует оба.
set(LAIUE_RENDER_HAS_D3D12 "${LAIUE_RENDER_HAS_D3D12}" PARENT_SCOPE)
set(LAIUE_RENDER_HAS_VULKAN_TARGET "${LAIUE_RENDER_HAS_VULKAN_TARGET}"
    PARENT_SCOPE)
