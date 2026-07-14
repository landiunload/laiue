cmake_minimum_required(VERSION 3.28)

project(laiue
    VERSION 0.4.0
    DESCRIPTION "Belaiu base - window, input, world, gameplay and render"
    LANGUAGES C
)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(LaiueToolchain)
include(LaiueModule)
include(LaiueShader)

add_subdirectory(src/platform)
add_subdirectory(src/input)
add_subdirectory(src/world)
add_subdirectory(src/mesh)
add_subdirectory(src/render)
add_subdirectory(src/core)

add_executable(laiue WIN32 src/launcher/main.c)
target_link_libraries(laiue PRIVATE laiue_common laiue_runtime kernel32)
target_link_options(laiue PRIVATE
    /ENTRY:EntryPoint
    /STACK:262144,4096
    /HEAP:65536,4096
)
