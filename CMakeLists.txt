cmake_minimum_required(VERSION 3.28)

# CMake 4+ can express MSVC runtime checks as a target property. The Windows
# runtime is intentionally no-CRT, so do not inject _RTC helper calls.
if(POLICY CMP0184)
    cmake_policy(SET CMP0184 NEW)
    set(CMAKE_MSVC_RUNTIME_CHECKS "")
endif()

project(laiue
    VERSION 0.5.0
    DESCRIPTION "Cross-platform voxel engine client/server runtime"
    LANGUAGES C
)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(WIN32)
    set(_laiue_client_default ON)
else()
    set(_laiue_client_default OFF)
endif()
option(LAIUE_BUILD_CLIENT "Собирать Windows-клиент" ${_laiue_client_default})
option(LAIUE_BUILD_SERVER "Собирать dedicated server" ON)
option(LAIUE_BUILD_EXAMPLE_MODS "Собирать примеры нативных модов" ON)

if(LAIUE_BUILD_CLIENT AND NOT WIN32)
    message(FATAL_ERROR
        "Linux-клиент пока не поддерживается; задайте LAIUE_BUILD_CLIENT=OFF")
endif()
if(NOT LAIUE_BUILD_CLIENT AND NOT LAIUE_BUILD_SERVER)
    message(FATAL_ERROR
        "Включите хотя бы LAIUE_BUILD_CLIENT или LAIUE_BUILD_SERVER")
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(LaiueToolchain)
include(LaiueModule)
include(LaiueShader)
include(LaiueMsQuic)
include(LaiueArchitecture)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # Imported dependency targets must be created in the common directory
    # scope so both platform_support and sibling headless modules can use
    # them.
    find_package(Threads REQUIRED)
    find_package(OpenSSL 3.0 REQUIRED COMPONENTS Crypto)
endif()

add_custom_target(laiue_architecture_check ALL
    COMMAND "${CMAKE_COMMAND}"
        "-DSOURCE_ROOT=${PROJECT_SOURCE_DIR}/src"
        -P "${PROJECT_SOURCE_DIR}/tools/check_architecture.cmake"
    COMMENT "Проверка направлений include-зависимостей"
    VERBATIM)
find_program(LAIUE_POWERSHELL_EXECUTABLE NAMES pwsh powershell)

# Launcher, DLL/SO и server лежат рядом. Для ELF модулей это сочетается с
# target-scoped $ORIGIN RPATH из LaiueModule.cmake.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib/$<CONFIG>")

# Общая OS-граница существует на обеих платформах. Window target внутри
# каталога создаётся только для Windows client build.
add_subdirectory(src/platform)

# Ровно этот набор является общим headless runtime для client и server.
add_subdirectory(src/world)
add_subdirectory(src/physics)
add_subdirectory(src/gameplay)
add_subdirectory(src/interaction)
add_subdirectory(src/content)
add_subdirectory(src/network)
add_subdirectory(src/mod)

get_property(LAIUE_HEADLESS_TARGETS GLOBAL PROPERTY
    LAIUE_RUNTIME_HEADLESS_TARGETS)
add_library(laiue_headless_stack INTERFACE)
add_library(laiue::headless_stack ALIAS laiue_headless_stack)
target_link_libraries(laiue_headless_stack
    INTERFACE ${LAIUE_HEADLESS_TARGETS})

if(LAIUE_BUILD_CLIENT)
    add_subdirectory(src/input)
    add_subdirectory(src/audio)
    add_subdirectory(src/mesh)
    add_subdirectory(src/render)
    add_subdirectory(src/core)
endif()

if(LAIUE_BUILD_SERVER)
    add_executable(laiue_server
        src/server/main.c
        src/server/server_config.c
        src/server/server_config.h)
    target_link_libraries(laiue_server PRIVATE
        laiue_common
        laiue::platform_support
        laiue::headless_stack)
    if(WIN32)
        target_link_options(laiue_server PRIVATE
            /ENTRY:ServerEntryPoint
            /SUBSYSTEM:CONSOLE
            /STACK:262144,4096
            /HEAP:65536,4096)
    else()
        set_target_properties(laiue_server PROPERTIES
            BUILD_RPATH "$ORIGIN"
            INSTALL_RPATH "$ORIGIN")
    endif()
    laiue_register_runtime_target(laiue_server SERVER)
endif()

if(LAIUE_BUILD_CLIENT)
    add_executable(laiue WIN32 src/launcher/main.c)
    target_link_libraries(laiue PRIVATE
        laiue_common
        laiue::platform_support
        kernel32)
    target_link_options(laiue PRIVATE
        /ENTRY:EntryPoint
        /STACK:262144,4096
        /HEAP:65536,4096)
    # Core загружается через LoadLibraryW, поэтому нужна build-, а не
    # link-зависимость.
    add_dependencies(laiue laiue_core)
    laiue_register_runtime_target(laiue CLIENT)
endif()

laiue_assert_build_architecture()

# Runtime assets раскладываются по ролям. Общие моды копируются один раз,
# server-only build не создаёт client-only каталоги shaders/textures/ui.
if(LAIUE_BUILD_CLIENT)
    set(_laiue_layout_anchor laiue)
else()
    set(_laiue_layout_anchor laiue_server)
endif()
add_custom_target(laiue_mod_content_files
    COMMAND "${CMAKE_COMMAND}" -E make_directory
        "$<TARGET_FILE_DIR:${_laiue_layout_anchor}>/mods"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
        "${PROJECT_SOURCE_DIR}/assets/mods"
        "$<TARGET_FILE_DIR:${_laiue_layout_anchor}>/mods"
    COMMENT "Подготовка общих runtime-модов")

if(LAIUE_BUILD_SERVER)
    add_custom_target(laiue_server_content_files
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${PROJECT_SOURCE_DIR}/assets/server.cfg"
            "$<TARGET_FILE_DIR:laiue_server>/server.cfg"
        COMMENT "Подготовка конфигурации dedicated server")
    add_dependencies(laiue_server_content_files
        laiue_mod_content_files)
endif()

if(LAIUE_BUILD_CLIENT)
    add_custom_target(laiue_client_content_files
        COMMAND "${CMAKE_COMMAND}" -E make_directory
            "$<TARGET_FILE_DIR:laiue>/shaders"
            "$<TARGET_FILE_DIR:laiue>/textures"
            "$<TARGET_FILE_DIR:laiue>/ui"
        COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "${PROJECT_SOURCE_DIR}/assets/texturepacks"
            "$<TARGET_FILE_DIR:laiue>/textures"
        COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "${PROJECT_SOURCE_DIR}/assets/shaderpacks"
            "$<TARGET_FILE_DIR:laiue>/shaders"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${PROJECT_SOURCE_DIR}/assets/ui/main_menu_background.png"
            "$<TARGET_FILE_DIR:laiue>/ui/main_menu_background.png"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${PROJECT_SOURCE_DIR}/assets/servers.txt"
            "$<TARGET_FILE_DIR:laiue>/servers.txt"
        COMMENT "Подготовка client assets")
    add_dependencies(laiue_client_content_files
        laiue_mod_content_files)
endif()

# Backward-compatible aggregate для локальных скриптов.
add_custom_target(laiue_content_files)
if(TARGET laiue_client_content_files)
    add_dependencies(laiue_content_files laiue_client_content_files)
endif()
if(TARGET laiue_server_content_files)
    add_dependencies(laiue_content_files laiue_server_content_files)
endif()

add_custom_target(laiue_sdk
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
        "${PROJECT_SOURCE_DIR}/sdk"
        "$<TARGET_FILE_DIR:${_laiue_layout_anchor}>/sdk"
    COMMENT "Копирование SDK модов")

if(LAIUE_BUILD_CLIENT)
    add_dependencies(laiue laiue_client_content_files laiue_sdk)
endif()
if(LAIUE_BUILD_SERVER)
    add_dependencies(laiue_server laiue_server_content_files)
endif()

# Локальная сборка примера объявляет в manifest ровно текущий артефакт.
# Официальный multi-platform pack агрегируется release pipeline из трёх
# платформенных stages.
function(laiue_add_example_mod mod_name)
    set(target_name "mod_${mod_name}")
    if(WIN32)
        set(native_stem "${mod_name}.windows-x86_64")
        set(LAIUE_MOD_NATIVE_ENTRIES
            "entry_windows_x86_64 = ${native_stem}.dll")
    elseif(LAIUE_LINUX_LIBC STREQUAL "musl")
        set(native_stem "${mod_name}.linux-x86_64-musl")
        set(LAIUE_MOD_NATIVE_ENTRIES
            "entry_linux_x86_64_musl = ${native_stem}.so")
    else()
        set(native_stem "${mod_name}.linux-x86_64-gnu")
        set(LAIUE_MOD_NATIVE_ENTRIES
            "entry_linux_x86_64_gnu = ${native_stem}.so")
    endif()

    set(manifest_template
        "${PROJECT_SOURCE_DIR}/sdk/examples/${mod_name}/mod.lm.in")
    set(generated_manifest
        "${CMAKE_CURRENT_BINARY_DIR}/generated-mods/${mod_name}/mod.lm")
    if(EXISTS "${manifest_template}")
        configure_file("${manifest_template}" "${generated_manifest}" @ONLY)
    else()
        # Переходный путь сохраняет configure во время миграции templates.
        set(generated_manifest
            "${PROJECT_SOURCE_DIR}/sdk/examples/${mod_name}/mod.lm")
    endif()

    add_library(${target_name} SHARED
        "sdk/examples/${mod_name}/${mod_name}.c")
    target_include_directories(${target_name} PRIVATE
        "${PROJECT_SOURCE_DIR}/sdk")
    target_link_libraries(${target_name} PRIVATE laiue_common)
    if(WIN32)
        target_link_options(${target_name} PRIVATE /NOENTRY)
    else()
        set_target_properties(${target_name} PROPERTIES
            C_VISIBILITY_PRESET hidden
            BUILD_RPATH "$ORIGIN"
            INSTALL_RPATH "$ORIGIN")
    endif()
    set(package_directory
        "${CMAKE_BINARY_DIR}/bin/$<CONFIG>/mods/${mod_name}.lmp")
    set_target_properties(${target_name} PROPERTIES
        PREFIX ""
        OUTPUT_NAME "${native_stem}"
        RUNTIME_OUTPUT_DIRECTORY "${package_directory}"
        LIBRARY_OUTPUT_DIRECTORY "${package_directory}")

    add_custom_target(${target_name}_manifest ALL
        COMMAND "${CMAKE_COMMAND}" -E make_directory
            "$<TARGET_FILE_DIR:${target_name}>"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${generated_manifest}"
            "$<TARGET_FILE_DIR:${target_name}>/mod.lm"
        DEPENDS ${target_name}
        COMMENT "Раскладка примера ${mod_name}.lmp")
    laiue_register_runtime_target(${target_name} MODSDK)
    set_property(GLOBAL APPEND PROPERTY LAIUE_EXAMPLE_MOD_TARGETS
        ${target_name} ${target_name}_manifest)

    install(TARGETS ${target_name}
        RUNTIME DESTINATION "mods/${mod_name}.lmp"
            COMPONENT ModSDK
        LIBRARY DESTINATION "mods/${mod_name}.lmp"
            COMPONENT ModSDK)
    install(FILES "${generated_manifest}"
        DESTINATION "mods/${mod_name}.lmp"
        COMPONENT ModSDK)
endfunction()

if(LAIUE_BUILD_EXAMPLE_MODS)
    laiue_add_example_mod(auto_bridge)
    laiue_add_example_mod(double_jump)
    laiue_add_example_mod(daylight_lock)
    laiue_add_example_mod(builder_lib)
    laiue_add_example_mod(spiral_tower)
endif()

# Динамический runtime target registry заменяет ручной список DLL.
if(WIN32 AND LAIUE_POWERSHELL_EXECUTABLE)
    add_custom_target(laiue_runtime_not_running_check
        COMMAND "${LAIUE_POWERSHELL_EXECUTABLE}" -NoProfile -File
            "${PROJECT_SOURCE_DIR}/tools/check_runtime_not_running.ps1"
            -RuntimeDirectory "$<TARGET_FILE_DIR:${_laiue_layout_anchor}>"
        COMMENT "Проверка, что runtime текущей сборки остановлен"
        VERBATIM)
    get_property(_laiue_runtime_targets GLOBAL PROPERTY LAIUE_RUNTIME_TARGETS)
    foreach(runtime_target IN LISTS _laiue_runtime_targets)
        add_dependencies(${runtime_target} laiue_runtime_not_running_check)
    endforeach()
endif()

get_property(_laiue_example_targets GLOBAL PROPERTY LAIUE_EXAMPLE_MOD_TARGETS)

include(GNUInstallDirs)
if(LAIUE_BUILD_SERVER)
    get_property(_laiue_server_targets GLOBAL PROPERTY
        LAIUE_RUNTIME_SERVER_TARGETS)
    install(TARGETS ${_laiue_server_targets} ${LAIUE_HEADLESS_TARGETS}
        RUNTIME DESTINATION "." COMPONENT Server
        LIBRARY DESTINATION "." COMPONENT Server)
    install(FILES "${PROJECT_SOURCE_DIR}/assets/server.cfg"
        DESTINATION "." COMPONENT Server)
endif()
if(LAIUE_BUILD_CLIENT)
    get_property(_laiue_client_targets GLOBAL PROPERTY
        LAIUE_RUNTIME_CLIENT_TARGETS)
    install(TARGETS ${_laiue_client_targets} ${LAIUE_HEADLESS_TARGETS}
        RUNTIME DESTINATION "." COMPONENT Client
        LIBRARY DESTINATION "." COMPONENT Client)
    install(FILES "${PROJECT_SOURCE_DIR}/assets/servers.txt"
        DESTINATION "." COMPONENT Client)
endif()
if(LAIUE_BUILD_SERVER)
    install(DIRECTORY "${PROJECT_SOURCE_DIR}/assets/mods/"
        DESTINATION "mods" COMPONENT Server)
endif()
if(LAIUE_BUILD_CLIENT)
    install(DIRECTORY "${PROJECT_SOURCE_DIR}/assets/mods/"
        DESTINATION "mods" COMPONENT Client)
    install(DIRECTORY "${PROJECT_SOURCE_DIR}/assets/shaderpacks/"
        DESTINATION "shaders" COMPONENT Client)
    install(DIRECTORY "${PROJECT_SOURCE_DIR}/assets/texturepacks/"
        DESTINATION "textures" COMPONENT Client)
    install(FILES
        "${PROJECT_SOURCE_DIR}/assets/ui/main_menu_background.png"
        DESTINATION "ui" COMPONENT Client)
endif()
install(DIRECTORY "${PROJECT_SOURCE_DIR}/sdk/"
    DESTINATION "sdk"
    COMPONENT ModSDK)
if(LAIUE_BUILD_SERVER)
    install(FILES "${PROJECT_SOURCE_DIR}/LICENSE"
        DESTINATION "." COMPONENT Server)
    install(FILES
        "${PROJECT_SOURCE_DIR}/docs/portability.md"
        "${PROJECT_SOURCE_DIR}/docs/secure_server.md"
        DESTINATION "docs" COMPONENT Server)
    install(FILES
        "${PROJECT_SOURCE_DIR}/packaging/linux/laiue-server.service"
        "${PROJECT_SOURCE_DIR}/packaging/linux/server.env.example"
        "${PROJECT_SOURCE_DIR}/packaging/linux/README.md"
        DESTINATION "deploy" COMPONENT Server)
endif()
if(LAIUE_BUILD_CLIENT)
    install(FILES "${PROJECT_SOURCE_DIR}/LICENSE"
        DESTINATION "." COMPONENT Client)
endif()
install(FILES "${PROJECT_SOURCE_DIR}/LICENSE"
    DESTINATION "." COMPONENT ModSDK)

if(TARGET laiue_msquic)
    function(laiue_install_msquic component_name)
        if(WIN32)
            install(IMPORTED_RUNTIME_ARTIFACTS laiue_msquic
                RUNTIME DESTINATION "." COMPONENT "${component_name}")
        else()
            # Copy only the selected library and its exact SONAME symlink
            # chain; never sweep unrelated libmsquic versions.
            install(CODE
                "file(COPY \"${LAIUE_MSQUIC_LIBRARY}\" DESTINATION \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}\" FOLLOW_SYMLINK_CHAIN)"
                COMPONENT "${component_name}")
        endif()
        if(LAIUE_MSQUIC_LICENSE)
            install(FILES "${LAIUE_MSQUIC_LICENSE}"
                DESTINATION "licenses/msquic"
                COMPONENT "${component_name}")
        endif()
        if(LAIUE_MSQUIC_NOTICE)
            install(FILES "${LAIUE_MSQUIC_NOTICE}"
                DESTINATION "licenses/msquic"
                COMPONENT "${component_name}")
        endif()
    endfunction()
    if(LAIUE_BUILD_SERVER)
        laiue_install_msquic(Server)
    endif()
    if(LAIUE_BUILD_CLIENT)
        laiue_install_msquic(Client)
    endif()
endif()

# Each bundle target stages one install component into its own directory.
# This prevents client/server artifacts from being inferred from a shared bin
# directory and makes stale files visible during package smoke tests.
function(laiue_add_component_bundle target_name directory_name
         component_name)
    add_custom_target(${target_name}
        COMMAND "${CMAKE_COMMAND}" -E rm -rf
            "${CMAKE_BINARY_DIR}/bundles/${directory_name}/$<CONFIG>"
        COMMAND "${CMAKE_COMMAND}" -E make_directory
            "${CMAKE_BINARY_DIR}/bundles/${directory_name}/$<CONFIG>"
        COMMAND "${CMAKE_COMMAND}" --install "${CMAKE_BINARY_DIR}"
            --config "$<CONFIG>"
            --prefix
                "${CMAKE_BINARY_DIR}/bundles/${directory_name}/$<CONFIG>"
            --component "${component_name}"
        VERBATIM)
    if(TARGET laiue_architecture_check)
        add_dependencies(${target_name} laiue_architecture_check)
    endif()
endfunction()

if(LAIUE_BUILD_CLIENT)
    laiue_add_component_bundle(
        laiue_client_bundle client Client)
    add_dependencies(laiue_client_bundle
        laiue laiue_core laiue_client_content_files laiue_sdk)
endif()
if(LAIUE_BUILD_SERVER)
    laiue_add_component_bundle(
        laiue_server_bundle server Server)
    add_dependencies(laiue_server_bundle
        laiue_server laiue_server_content_files)
endif()
laiue_add_component_bundle(
    laiue_modsdk_bundle mod-sdk ModSDK)
add_dependencies(laiue_modsdk_bundle laiue_sdk)
if(_laiue_example_targets)
    add_dependencies(laiue_modsdk_bundle ${_laiue_example_targets})
endif()

add_custom_target(laiue_distribution)
add_dependencies(laiue_distribution laiue_modsdk_bundle)
if(TARGET laiue_client_bundle)
    add_dependencies(laiue_distribution laiue_client_bundle)
endif()
if(TARGET laiue_server_bundle)
    add_dependencies(laiue_distribution laiue_server_bundle)
endif()

set(CPACK_PACKAGE_NAME "laiue")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
if(WIN32)
    set(CPACK_GENERATOR ZIP)
else()
    set(CPACK_GENERATOR TGZ)
endif()
include(CPack)

include(CTest)
if(BUILD_TESTING)
    add_test(NAME laiue.architecture
        COMMAND "${CMAKE_COMMAND}"
            "-DSOURCE_ROOT=${PROJECT_SOURCE_DIR}/src"
            -P "${PROJECT_SOURCE_DIR}/tools/check_architecture.cmake")
    add_subdirectory(tests)
endif()
