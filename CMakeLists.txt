cmake_minimum_required(VERSION 3.28)

project(laiue
    VERSION 0.5.0
    DESCRIPTION "Windows voxel engine with client/server runtime"
    LANGUAGES C
)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(LaiueToolchain)
include(LaiueModule)
include(LaiueShader)

find_program(LAIUE_POWERSHELL_EXECUTABLE NAMES pwsh powershell)
if(LAIUE_POWERSHELL_EXECUTABLE)
    add_custom_target(laiue_architecture_check
        COMMAND "${LAIUE_POWERSHELL_EXECUTABLE}" -NoProfile -File
            "${PROJECT_SOURCE_DIR}/tools/check_architecture.ps1"
        COMMENT "Проверка направлений include-зависимостей"
        VERBATIM
    )
else()
    message(WARNING "PowerShell не найден: проверка архитектурных границ отключена")
endif()

# Launcher и все загружаемые им DLL должны лежать рядом. Генераторное
# выражение одновременно разделяет Debug/Release и не даёт Visual Studio
# добавить к пути ещё один каталог конфигурации.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>")

add_subdirectory(src/platform)
add_subdirectory(src/input)
add_subdirectory(src/world)
add_subdirectory(src/mesh)
add_subdirectory(src/content)
add_subdirectory(src/mod)
add_subdirectory(src/network)
add_subdirectory(src/render)
add_subdirectory(src/physics)
add_subdirectory(src/gameplay)
add_subdirectory(src/interaction)
add_subdirectory(src/core)

# Dedicated server has no dependency on window, input, renderer, mesher or
# client core. content/mod нужны для серверных модов и разрешённых bundle.
add_executable(laiue_server
    src/server/main.c
    src/server/server_config.c
    src/server/server_config.h)
target_link_libraries(laiue_server PRIVATE
    laiue_common
    laiue_runtime
    laiue_network
    laiue_content
    laiue_mod
    laiue_world
    laiue_physics
    laiue_gameplay
    laiue_interaction
    kernel32
)
target_link_options(laiue_server PRIVATE
    /ENTRY:ServerEntryPoint
    /SUBSYSTEM:CONSOLE
    /STACK:262144,4096
    /HEAP:65536,4096
)

add_executable(laiue WIN32 src/launcher/main.c)
target_link_libraries(laiue PRIVATE laiue_common laiue_runtime kernel32)
# Core загружается через LoadLibraryW, поэтому обычной link-зависимости у
# launcher нет. Явная build-зависимость собирает core и весь его граф DLL
# даже при сборке только цели laiue.
# Канонические каталоги пользовательского содержимого. Форматы и каталоги
# описаны единым модулем laiue_content; CMake только готовит runtime-структуру.
add_custom_target(laiue_content_files
    COMMAND "${CMAKE_COMMAND}" -E make_directory
        "$<TARGET_FILE_DIR:laiue>/mods"
        "$<TARGET_FILE_DIR:laiue>/shaders"
        "$<TARGET_FILE_DIR:laiue>/textures"
        "$<TARGET_FILE_DIR:laiue>/ui"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
        "${PROJECT_SOURCE_DIR}/assets/texturepacks"
        "$<TARGET_FILE_DIR:laiue>/textures"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
        "${PROJECT_SOURCE_DIR}/assets/mods"
        "$<TARGET_FILE_DIR:laiue>/mods"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
        "${PROJECT_SOURCE_DIR}/assets/shaderpacks"
        "$<TARGET_FILE_DIR:laiue>/shaders"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${PROJECT_SOURCE_DIR}/assets/ui/main_menu_background.png"
        "$<TARGET_FILE_DIR:laiue>/ui/main_menu_background.png"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${PROJECT_SOURCE_DIR}/assets/server.cfg"
        "$<TARGET_FILE_DIR:laiue>/server.cfg"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${PROJECT_SOURCE_DIR}/assets/servers.txt"
        "$<TARGET_FILE_DIR:laiue>/servers.txt"
    COMMENT "Подготовка модов, шейдеров и текстур"
)
add_dependencies(laiue laiue_core laiue_content_files laiue_server)
if(TARGET laiue_architecture_check)
    add_dependencies(laiue laiue_architecture_check)
endif()
target_link_options(laiue PRIVATE
    /ENTRY:EntryPoint
    /STACK:262144,4096
    /HEAP:65536,4096
)

# Windows не позволяет линкеру заменить загруженную DLL. Проверяем runtime
# до компиляции/линковки и выдаём понятный PID вместо позднего permission denied.
if(LAIUE_POWERSHELL_EXECUTABLE)
    add_custom_target(laiue_runtime_not_running_check
        COMMAND "${LAIUE_POWERSHELL_EXECUTABLE}" -NoProfile -File
            "${PROJECT_SOURCE_DIR}/tools/check_runtime_not_running.ps1"
            -RuntimeDirectory "$<TARGET_FILE_DIR:laiue>"
        COMMENT "Проверка, что клиент и сервер текущей сборки остановлены"
        VERBATIM
    )
    set(laiue_runtime_targets
        laiue
        laiue_server
        laiue_window
        laiue_input
        laiue_world
        laiue_mesher
        laiue_content
        laiue_mod
        laiue_network
        laiue_render
        laiue_physics
        laiue_gameplay
        laiue_interaction
        laiue_core
    )
    foreach(runtime_target IN LISTS laiue_runtime_targets)
        add_dependencies(${runtime_target} laiue_runtime_not_running_check)
    endforeach()
endif()

# SDK модов кладётся рядом с игрой: заголовок API и примеры с исходниками —
# мод пишется и собирается без исходников движка (docs/modding.md).
add_custom_target(laiue_sdk
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
        "${PROJECT_SOURCE_DIR}/sdk"
        "$<TARGET_FILE_DIR:laiue>/sdk"
    COMMENT "Копирование SDK модов"
)
add_dependencies(laiue laiue_sdk)

# Примеры нативных DLL-модов собираются как настоящие моды: без линка
# с движком, только заголовок SDK. laiue_runtime даёт _fltused
# (в нашем дереве без CRT); внешняя сборка обходится обычным cl /LD.
# Манифест копируется отдельной целью: связка «линк + копия» в одном
# ninja-правиле рождает вложенный cmd, ломкий в урезанных окружениях.
function(laiue_add_example_mod mod_name)
    set(target_name "mod_${mod_name}")
    add_library(${target_name} SHARED
        "sdk/examples/${mod_name}/${mod_name}.c")
    target_include_directories(${target_name} PRIVATE
        "${PROJECT_SOURCE_DIR}/sdk")
    target_link_libraries(${target_name} PRIVATE
        laiue_common laiue_runtime kernel32)
    target_link_options(${target_name} PRIVATE /NOENTRY)
    if(TARGET laiue_runtime_not_running_check)
        add_dependencies(${target_name} laiue_runtime_not_running_check)
    endif()
    set_target_properties(${target_name} PROPERTIES
        OUTPUT_NAME ${mod_name}
        RUNTIME_OUTPUT_DIRECTORY
            "${CMAKE_BINARY_DIR}/bin/$<CONFIG>/mods/${mod_name}.lmp")
    add_custom_target(${target_name}_manifest
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${PROJECT_SOURCE_DIR}/sdk/examples/${mod_name}/mod.lm"
            "$<TARGET_FILE_DIR:${target_name}>/mod.lm"
        COMMENT "Раскладка примера ${mod_name}.lmp"
    )
    add_dependencies(${target_name}_manifest ${target_name})
    add_dependencies(laiue ${target_name} ${target_name}_manifest)
endfunction()

laiue_add_example_mod(auto_bridge)
laiue_add_example_mod(double_jump)
laiue_add_example_mod(daylight_lock)
laiue_add_example_mod(builder_lib)
laiue_add_example_mod(spiral_tower)
