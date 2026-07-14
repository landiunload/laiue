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

# Launcher и все загружаемые им DLL должны лежать рядом. Генераторное
# выражение одновременно разделяет Debug/Release и не даёт Visual Studio
# добавить к пути ещё один каталог конфигурации.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>")

add_subdirectory(src/platform)
add_subdirectory(src/input)
add_subdirectory(src/world)
add_subdirectory(src/mesh)
add_subdirectory(src/render)
add_subdirectory(src/physics)
add_subdirectory(src/gameplay)
add_subdirectory(src/interaction)
add_subdirectory(src/core)

add_executable(laiue WIN32 src/launcher/main.c)
target_link_libraries(laiue PRIVATE laiue_common laiue_runtime kernel32)
# Core загружается через LoadLibraryW, поэтому обычной link-зависимости у
# launcher нет. Явная build-зависимость собирает core и весь его граф DLL
# даже при сборке только цели laiue.
add_custom_target(laiue_texturepacks
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
        "${PROJECT_SOURCE_DIR}/assets/texturepacks"
        "$<TARGET_FILE_DIR:laiue>/texturepacks"
    COMMENT "Копирование текстурпаков"
)
add_dependencies(laiue laiue_core laiue_texturepacks)
target_link_options(laiue PRIVATE
    /ENTRY:EntryPoint
    /STACK:262144,4096
    /HEAP:65536,4096
)
