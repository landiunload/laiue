add_library(laiue_mesh_world_demo_common STATIC mesh_world_demo_world.c mesh_world_demo_world.h)
target_include_directories(laiue_mesh_world_demo_common PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${PROJECT_SOURCE_DIR}/src"
    "${PROJECT_SOURCE_DIR}/src/assets"
    "${PROJECT_SOURCE_DIR}/src/simulation"
    "${PROJECT_SOURCE_DIR}/src/graphics"
    "${PROJECT_SOURCE_DIR}/src/modding")
target_link_libraries(laiue_mesh_world_demo_common PRIVATE
    laiue::build_options laiue::math_support)
target_compile_definitions(laiue_mesh_world_demo_common PRIVATE LAIUE_STATIC=1)

add_executable(laiue_mesh_world_demo mesh_world_demo.c)
target_link_libraries(laiue_mesh_world_demo PRIVATE
    laiue_bootstrap laiue_mesh_world_demo_common laiue::math_support)
target_compile_definitions(laiue_mesh_world_demo PRIVATE LAIUE_STATIC=1)

foreach(_module content model mesh_world mesh_world_render)
    if(TARGET laiue_${_module})
        if(LAIUE_MODULE_LIBRARY_TYPE_RESOLVED STREQUAL "STATIC")
            target_link_libraries(laiue_mesh_world_demo PRIVATE laiue_${_module})
            string(TOUPPER "${_module}" _definition)
            target_compile_definitions(laiue_mesh_world_demo PRIVATE
                "LAIUE_MESH_DEMO_STATIC_${_definition}=1")
        else()
            add_dependencies(laiue_mesh_world_demo laiue_${_module})
        endif()
    endif()
endforeach()
if(NOT LAIUE_MODULE_LIBRARY_TYPE_RESOLVED STREQUAL "STATIC")
    target_compile_definitions(laiue_mesh_world_demo PRIVATE LAIUE_MESH_DEMO_DYNAMIC=1)
endif()

if(TARGET laiue_render)
    target_compile_definitions(laiue_mesh_world_demo PRIVATE LAIUE_MESH_DEMO_GRAPHICS=1)
    if(LAIUE_MODULE_LIBRARY_TYPE_RESOLVED STREQUAL "STATIC")
        target_link_libraries(laiue_mesh_world_demo PRIVATE laiue_render)
    else()
        set(_graphics laiue_render)
        if(LAIUE_RENDER_BACKEND_RESOLVED STREQUAL "D3D12" AND TARGET laiue_graphics_d3d12)
            set(_graphics laiue_graphics_d3d12)
            target_compile_definitions(laiue_mesh_world_demo PRIVATE
                LAIUE_MESH_DEMO_GRAPHICS_D3D12=1)
        elseif(TARGET laiue_graphics_vulkan)
            set(_graphics laiue_graphics_vulkan)
            target_compile_definitions(laiue_mesh_world_demo PRIVATE
                LAIUE_MESH_DEMO_GRAPHICS_VULKAN=1)
        endif()
        add_dependencies(laiue_mesh_world_demo ${_graphics})
        if(TARGET laiue_graphics_vulkan)
            add_dependencies(laiue_mesh_world_demo laiue_graphics_vulkan)
            target_compile_definitions(laiue_mesh_world_demo PRIVATE
                LAIUE_MESH_DEMO_OFFSCREEN_VULKAN=1)
        endif()
    endif()
    if(LAIUE_RENDER_HAS_VULKAN_TARGET)
        target_compile_definitions(laiue_mesh_world_demo PRIVATE
            LAIUE_MESH_DEMO_OFFSCREEN_VULKAN=1)
    endif()
endif()

foreach(_module window input)
    if(TARGET laiue_${_module})
        target_compile_definitions(laiue_mesh_world_demo PRIVATE LAIUE_MESH_DEMO_WINDOWED=1)
        if(LAIUE_MODULE_LIBRARY_TYPE_RESOLVED STREQUAL "STATIC")
            target_link_libraries(laiue_mesh_world_demo PRIVATE laiue_${_module})
        else()
            add_dependencies(laiue_mesh_world_demo laiue_${_module})
        endif()
    endif()
endforeach()

# Source assets are copied into the build output. Runtime caches, if written,
# therefore live beside the copies and never change the source tree.
add_custom_target(laiue_mesh_world_demo_assets
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/assets"
        "$<TARGET_FILE_DIR:laiue_mesh_world_demo>/assets_mesh_world"
    VERBATIM)
add_dependencies(laiue_mesh_world_demo laiue_mesh_world_demo_assets)
laiue_configure_standalone_executable(laiue_mesh_world_demo MeshWorldDemoEntryPoint)
