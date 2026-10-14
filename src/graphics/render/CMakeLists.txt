set(render_shader_headers)
foreach(shader IN ITEMS chunk panorama ui generic)
    laiue_compile_shader(${shader}
        SOURCE "${PROJECT_SOURCE_DIR}/shaders/${shader}.hlsl"
        OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated"
        FALLBACK_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/generated"
        OUTPUT_VARIABLE shader_headers)
    list(APPEND render_shader_headers ${shader_headers})
endforeach()

set(render_sources
    renderer_dispatch.c
    renderer.h
    renderer_internal.h
    graphics_service.h
    module_entry.c
    content_provider.c
    content_provider.h
    ui_quad.h
    shader_pack.c
    shader_pack.h
    texture_build.c
    texture_pack.c
    texture_pack.h
    texture_pack_internal.h
    chunk_geometry.h
    ../../api.h
    ${render_shader_headers})
set(d3d12_sources renderer_d3d12.c ui_image_wic.c ui_image_wic.h)
set(d3d12_links d3d12 dxgi dxguid ole32 windowscodecs)
set(vulkan_sources renderer_vulkan.c renderer_offscreen.h)
set(vulkan_links Vulkan::Vulkan)

# Физически доступные бэкенды. AUTO (умолчание) линкует всё доступное, а
# явный LAIUE_RENDER_BACKEND — ровно один бэкенд. Сравнивается исходная
# опция: на Windows AUTO тоже разрешается в D3D12, и по
# LAIUE_RENDER_BACKEND_RESOLVED их не отличить.
find_package(Vulkan QUIET)
find_package(X11 QUIET)
string(TOUPPER "${LAIUE_RENDER_BACKEND}" requested_backend)
set(LAIUE_RENDER_HAS_D3D12 OFF)
set(LAIUE_RENDER_HAS_VULKAN_TARGET OFF)
if(requested_backend STREQUAL "AUTO")
    if(WIN32)
        set(LAIUE_RENDER_HAS_D3D12 ON)
    endif()
    if(Vulkan_FOUND)
        set(LAIUE_RENDER_HAS_VULKAN_TARGET ON)
    endif()
elseif(requested_backend STREQUAL "D3D12")
    set(LAIUE_RENDER_HAS_D3D12 ON)
elseif(requested_backend STREQUAL "VULKAN")
    if(Vulkan_FOUND)
        set(LAIUE_RENDER_HAS_VULKAN_TARGET ON)
    endif()
else()
    message(FATAL_ERROR "LAIUE_RENDER_BACKEND must be AUTO, D3D12 or VULKAN")
endif()

set(backend_sources)
set(backend_links)
set(backend_definitions)
if(LAIUE_RENDER_HAS_D3D12)
    list(APPEND backend_sources ${d3d12_sources})
    list(APPEND backend_links ${d3d12_links})
    list(APPEND backend_definitions LAIUE_RENDER_HAS_D3D12)
endif()
if(LAIUE_RENDER_HAS_VULKAN_TARGET)
    list(APPEND backend_sources ${vulkan_sources})
    list(APPEND backend_links ${vulkan_links})
    list(APPEND backend_definitions LAIUE_RENDER_HAS_VULKAN)
    if(X11_FOUND)
        list(APPEND backend_links X11::X11)
        list(APPEND backend_definitions LAIUE_RENDER_HAS_X11)
    endif()
endif()
if(NOT backend_definitions)
    message(FATAL_ERROR
        "No render backend available: need Windows (D3D12) or a Vulkan SDK.")
endif()

# RendererCreate()/RENDERER_BACKEND_AUTO выбирает бэкенд, разрешённый из
# опции сборки: на Windows D3D12, иначе Vulkan.
if(LAIUE_RENDER_BACKEND_RESOLVED STREQUAL "D3D12")
    list(APPEND backend_definitions LAIUE_RENDER_DEFAULT_BACKEND_D3D12)
else()
    list(APPEND backend_definitions LAIUE_RENDER_DEFAULT_BACKEND_VULKAN)
endif()

laiue_add_module(render
    RUNTIME_ROLE GRAPHICS
    SOURCES ${render_sources} ${backend_sources}
    LINK ${backend_links} laiue::media_support)
target_compile_definitions(laiue_render PRIVATE ${backend_definitions})
# Бэкенд включает render/generated/<backend>/*.h. Binary include идёт
# первым, поэтому обычная сборка никогда не перезаписывает checked-in
# fallback.
target_include_directories(laiue_render BEFORE PRIVATE "${PROJECT_BINARY_DIR}/src")

# `laiue_render` may contain both backends so existing applications keep one
# DLL. Each provider below compiles exactly one native backend and has no
# link-time dependency on the other. Both publish `laiue.graphics` with
# distinct descriptor IDs, so a profile selects one explicitly.
option(LAIUE_BUILD_SEPARATE_GRAPHICS_PROVIDERS
    "Build standalone D3D12 and Vulkan graphics provider modules" ON)
if(LAIUE_BUILD_SEPARATE_GRAPHICS_PROVIDERS AND
   LAIUE_MODULE_LIBRARY_TYPE_RESOLVED STREQUAL "SHARED")
    set(providers)
    if(WIN32)
        list(APPEND providers d3d12)
    endif()
    if(Vulkan_FOUND)
        list(APPEND providers vulkan)
    endif()
    foreach(provider IN LISTS providers)
        string(TOUPPER "${provider}" provider_upper)
        set(provider_target laiue_graphics_${provider})
        laiue_add_module(graphics_${provider}
            RUNTIME_ROLE GRAPHICS
            SOURCES ${render_sources} ${${provider}_sources}
            LINK ${${provider}_links} laiue::media_support)
        target_compile_definitions(${provider_target} PRIVATE
            LAIUE_BUILD_RENDER
            LAIUE_RENDER_HAS_${provider_upper}
            LAIUE_RENDER_DEFAULT_BACKEND_${provider_upper}
            "LAIUE_RENDER_PROVIDER_ID=\"laiue.graphics.${provider}\"")
        if(provider STREQUAL "vulkan" AND X11_FOUND)
            target_link_libraries(${provider_target} PRIVATE X11::X11)
            target_compile_definitions(${provider_target} PRIVATE LAIUE_RENDER_HAS_X11)
        endif()
        target_include_directories(${provider_target} BEFORE PRIVATE
            "${PROJECT_BINARY_DIR}/src")
    endforeach()
endif()

# Тесты решают, какие прогоны регистрировать, по реально слинкованным
# бэкендам: на Windows AUTO неотличим от явного D3D12 по resolved-значению.
set(LAIUE_RENDER_HAS_D3D12 "${LAIUE_RENDER_HAS_D3D12}" PARENT_SCOPE)
set(LAIUE_RENDER_HAS_VULKAN_TARGET "${LAIUE_RENDER_HAS_VULKAN_TARGET}" PARENT_SCOPE)
