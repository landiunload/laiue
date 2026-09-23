cmake_minimum_required(VERSION 3.28)

if(POLICY CMP0184)
    cmake_policy(SET CMP0184 NEW)
    set(CMAKE_MSVC_RUNTIME_CHECKS "")
endif()
if(POLICY CMP0141)
    # Отладочная информация встраивается в объектный файл (/Z7), а не
    # пишется в общий PDB: no-CRT сборке не нужен отдельный файл, а
    # компиляция не сериализуется на записи в него. Формат задаётся один
    # раз политикой; иначе CMake добавил бы свой /Zi и компилятор ругался
    # бы на переопределение в каждой единице трансляции.
    cmake_policy(SET CMP0141 NEW)
    set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT
        "$<$<CONFIG:Debug,RelWithDebInfo>:Embedded>")
endif()

project(laiue
    VERSION 0.7.0
    DESCRIPTION "C17 technology modules with infinite coordinates"
    LANGUAGES C)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(LaiuePlatform)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(LAIUE_PLATFORM_WINDOWS)
    set(_laiue_graphics_default ON)
else()
    set(_laiue_graphics_default OFF)
endif()
option(LAIUE_BUILD_GRAPHICS
    "Build the window/input/mesher/render/scene graphics layer"
    ${_laiue_graphics_default})

# Audio and UI are technology modules in their own right.  UI does not require
# a renderer at compile time, but its runtime service graph explicitly
# requires laiue.graphics; headless profiles can still build it and omit it
# from the selected module profile.
option(LAIUE_BUILD_AUDIO "Build the audio mixer and system/offscreen output" ON)
option(LAIUE_BUILD_AUDIO_PACK "Build the optional audio pack/decoder module" ON)
option(LAIUE_BUILD_UI "Build the backend-neutral UI module" ON)

# Набор графических модулей задаёт бэкенд, а не платформа. D3D12 несёт
# полный Windows-слой; Vulkan остаётся переносимым и поддерживает offscreen
# без хендла, а на Windows с HWND — Win32 swapchain. Оконный, вводный и UI
# слои Vulkan-профиль не включает; звук переносим целиком, а вывод у него
# свой на каждой платформе. Список выводится в конфигурации, чтобы
# неполнота профиля была видна, а не подразумевалась.
if(LAIUE_RENDER_BACKEND_RESOLVED STREQUAL "D3D12")
    set(LAIUE_GRAPHICS_MODULE_DIRECTORIES input mesh render scene voxel_render)
else()
    set(LAIUE_GRAPHICS_MODULE_DIRECTORIES mesh render scene voxel_render)
endif()

include(LaiueToolchain)
include(LaiueModule)
include(LaiueShader)
include(LaiueArchitecture)
include(LaiueStandaloneExecutable)

if(LAIUE_BUILD_GRAPHICS AND NOT (LAIUE_TARGET_X86_64 OR LAIUE_TARGET_ARM64))
    message(FATAL_ERROR
        "The graphics implementation supports x86_64 and ARM64; "
        "use LAIUE_BUILD_GRAPHICS=OFF for the portable core")
endif()
if(LAIUE_BUILD_GRAPHICS)
    message(STATUS
        "laiue graphics modules (${LAIUE_RENDER_BACKEND_RESOLVED}): "
        "${LAIUE_GRAPHICS_MODULE_DIRECTORIES}")
endif()

add_custom_target(laiue_architecture_check ALL
    COMMAND "${CMAKE_COMMAND}"
        "-DSOURCE_ROOT=${PROJECT_SOURCE_DIR}/src"
        -P "${PROJECT_SOURCE_DIR}/tools/check_architecture.cmake"
    COMMENT "Checking module include boundaries"
    VERBATIM)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib/$<CONFIG>")

add_subdirectory(src/core/math)
add_subdirectory(src/numeric)
add_subdirectory(src/assets/media)
add_subdirectory(src/platform)
add_subdirectory(src/jobs/task)
add_subdirectory(src/simulation/world)
add_subdirectory(src/simulation/physics)
add_subdirectory(src/assets/content)
add_subdirectory(src/simulation/character)
add_subdirectory(src/simulation/voxel)
add_subdirectory(src/modding/mod)
if(LAIUE_BUILD_AUDIO OR LAIUE_BUILD_AUDIO_PACK)
    add_subdirectory(src/audio)
endif()
if(LAIUE_BUILD_UI)
    add_subdirectory(src/ui)
endif()

# Minimal application bootstrap. It contains only the standalone module
# resolver and the platform boundary; it deliberately does not link the
# legacy aggregate `laiue_engine` or any graphics/physics/voxel library. An
# application can therefore embed this static archive and choose an explicit
# module graph at runtime. The existing `laiue::mod` target remains the
# compatibility aggregate for old native mods.
add_library(laiue_bootstrap STATIC
    src/modding/mod/module_api.h
    src/modding/mod/module_manifest.c
    src/modding/mod/module_manifest.h
    src/modding/mod/module_host.c
    src/modding/mod/module_host.h
    src/modding/mod/mod_internal.c
    src/modding/mod/mod_internal.h
    src/modding/mod/mod_types.h)
add_library(laiue::bootstrap ALIAS laiue_bootstrap)
target_include_directories(laiue_bootstrap
    PUBLIC
        "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>"
        "$<INSTALL_INTERFACE:include/laiue>")
target_link_libraries(laiue_bootstrap
    PRIVATE
        laiue_common
        laiue::platform_support)
target_compile_definitions(laiue_bootstrap PRIVATE LAIUE_STATIC=1)
set_target_properties(laiue_bootstrap PROPERTIES
    EXPORT_NAME bootstrap
    POSITION_INDEPENDENT_CODE ON)

# Public contracts for optional technology providers. These are intentionally
# header-only interface targets: applications can compile a provider without
# pulling the voxel, renderer, or physics implementation into the process.
add_library(laiue_graphics_api INTERFACE)
add_library(laiue::graphics_api ALIAS laiue_graphics_api)
target_include_directories(laiue_graphics_api INTERFACE
    "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>"
    "$<INSTALL_INTERFACE:include/laiue>")
set_target_properties(laiue_graphics_api PROPERTIES EXPORT_NAME graphics_api)

add_library(laiue_voxel_api INTERFACE)
add_library(laiue::voxel_api ALIAS laiue_voxel_api)
target_include_directories(laiue_voxel_api INTERFACE
    "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>"
    "$<INSTALL_INTERFACE:include/laiue>")
set_target_properties(laiue_voxel_api PROPERTIES EXPORT_NAME voxel_api)

add_library(laiue_character_api INTERFACE)
add_library(laiue::character_api ALIAS laiue_character_api)
target_include_directories(laiue_character_api INTERFACE
    "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>"
    "$<INSTALL_INTERFACE:include/laiue>")
set_target_properties(laiue_character_api PROPERTIES EXPORT_NAME character_api)

if(LAIUE_BUILD_GRAPHICS)
    foreach(graphics_directory IN LISTS LAIUE_GRAPHICS_MODULE_DIRECTORIES)
        add_subdirectory("src/graphics/${graphics_directory}")
    endforeach()
endif()

get_property(LAIUE_RUNTIME_TARGETS GLOBAL PROPERTY LAIUE_RUNTIME_TARGETS)
add_library(laiue_engine INTERFACE)
add_library(laiue::engine ALIAS laiue_engine)
target_link_libraries(laiue_engine INTERFACE ${LAIUE_RUNTIME_TARGETS})
set_target_properties(laiue_engine PROPERTIES EXPORT_NAME engine)

# A static archive can hide unresolved references in object files that no
# consumer happens to pull in.  Every external static profile therefore
# performs one final link with every engine-core object.
if(LAIUE_PLATFORM_EXTERNAL AND
   LAIUE_MODULE_LIBRARY_TYPE_RESOLVED STREQUAL "STATIC")
    string(CONCAT _laiue_static_core_link
        "$<LINK_LIBRARY:WHOLE_ARCHIVE,"
        "laiue_numeric,laiue_world,laiue_physics,laiue_task,laiue_content,laiue_character,laiue_voxel,laiue_mod>")
    add_executable(laiue_static_core_link_smoke
        tests/static_core_link_smoke.c)
    target_link_libraries(laiue_static_core_link_smoke PRIVATE
        laiue_common
        "${_laiue_static_core_link}")
    if(APPLE)
        set_target_properties(laiue_static_core_link_smoke PROPERTIES
            MACOSX_BUNDLE TRUE
            MACOSX_BUNDLE_BUNDLE_NAME "LaiueCoreLinkSmoke"
            MACOSX_BUNDLE_GUI_IDENTIFIER "dev.laiue.core-link-smoke"
            OUTPUT_NAME "LaiueCoreLinkSmoke"
            XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED "NO")
    endif()
endif()

laiue_assert_build_architecture()

if(LAIUE_ENABLE_SDK_INSTALL_RESOLVED)
    include(GNUInstallDirs)
    include(CMakePackageConfigHelpers)

install(TARGETS ${LAIUE_RUNTIME_TARGETS} laiue_engine
        laiue_graphics_api laiue_voxel_api laiue_character_api
    EXPORT laiueTargets
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT Engine
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT Engine
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT Engine
    INCLUDES DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/laiue"
)

# The bootstrap archive intentionally keeps its private platform/no-CRT link
# implementation out of the public aggregate export. It is still staged with
# the SDK and can be linked directly by applications built in the same
# toolchain; the package export for the legacy aggregate remains unchanged.
install(TARGETS laiue_bootstrap
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT Engine
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT Engine
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT Engine)
install(TARGETS laiue_platform_support
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT Engine
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT Engine
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT Engine)

install(FILES "${PROJECT_SOURCE_DIR}/src/api.h"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/laiue"
    COMPONENT Engine)
install(FILES "${PROJECT_SOURCE_DIR}/src/numeric/infinite_coord.h"
        "${PROJECT_SOURCE_DIR}/src/numeric/numeric_service.h"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/laiue/numeric"
    COMPONENT Engine)
install(FILES
    "${PROJECT_SOURCE_DIR}/src/simulation/world/world.h"
    "${PROJECT_SOURCE_DIR}/src/simulation/world/world_service.h"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/laiue/world"
    COMPONENT Engine)
install(FILES
        "${PROJECT_SOURCE_DIR}/src/simulation/physics/rigid_body.h"
        "${PROJECT_SOURCE_DIR}/src/simulation/physics/compound_shape.h"
        "${PROJECT_SOURCE_DIR}/src/simulation/physics/rigid_broadphase.h"
        "${PROJECT_SOURCE_DIR}/src/simulation/physics/voxel_body.h"
        "${PROJECT_SOURCE_DIR}/src/simulation/physics/physics_service.h"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/laiue/physics"
    COMPONENT Engine)
install(FILES "${PROJECT_SOURCE_DIR}/src/jobs/task/task_pool.h"
        "${PROJECT_SOURCE_DIR}/src/jobs/task/task_service.h"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/laiue/task"
    COMPONENT Engine)
install(FILES
        "${PROJECT_SOURCE_DIR}/src/assets/content/content_catalog.h"
        "${PROJECT_SOURCE_DIR}/src/assets/content/content_format.h"
        "${PROJECT_SOURCE_DIR}/src/assets/content/content_service.h"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/laiue/content"
    COMPONENT Engine)
install(FILES
        "${PROJECT_SOURCE_DIR}/src/modding/mod/mod.h"
        "${PROJECT_SOURCE_DIR}/src/modding/mod/mod_api.h"
        "${PROJECT_SOURCE_DIR}/src/modding/mod/mod_host.h"
        "${PROJECT_SOURCE_DIR}/src/modding/mod/mod_manifest.h"
        "${PROJECT_SOURCE_DIR}/src/modding/mod/mod_types.h"
        "${PROJECT_SOURCE_DIR}/src/modding/mod/module_api.h"
        "${PROJECT_SOURCE_DIR}/src/modding/mod/module_service.h"
        "${PROJECT_SOURCE_DIR}/src/modding/mod/module_manifest.h"
        "${PROJECT_SOURCE_DIR}/src/modding/mod/module_host.h"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/laiue/mod"
    COMPONENT Engine)
install(FILES "${PROJECT_SOURCE_DIR}/src/graphics/graphics_api.h"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/laiue/graphics"
    COMPONENT Engine)
install(FILES "${PROJECT_SOURCE_DIR}/src/simulation/voxel/voxel_api.h"
        "${PROJECT_SOURCE_DIR}/src/simulation/voxel/voxel_service.h"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/laiue/voxel"
    COMPONENT Engine)
install(FILES
        "${PROJECT_SOURCE_DIR}/src/simulation/character/character_api.h"
        "${PROJECT_SOURCE_DIR}/src/simulation/character/character_service.h"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/laiue/character"
    COMPONENT Engine)
# Заголовки ставятся по собранным целям, а не по одному флагу графики:
# набор графических модулей зависит от бэкенда, и SDK не должен обещать
# заголовок модуля, которого в сборке нет.
if(TARGET laiue_window)
    install(FILES "${PROJECT_SOURCE_DIR}/src/platform/time.h"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/laiue/platform"
        COMPONENT Engine)
endif()
if(TARGET laiue_audio)
    install(FILES
            "${PROJECT_SOURCE_DIR}/src/audio/audio.h"
            "${PROJECT_SOURCE_DIR}/src/audio/audio_offscreen.h"
            "${PROJECT_SOURCE_DIR}/src/audio/audio_service.h"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/laiue/audio"
        COMPONENT Engine)
endif()
if(TARGET laiue_audio_pack)
    install(FILES
            "${PROJECT_SOURCE_DIR}/src/audio/audio_pack.h"
            "${PROJECT_SOURCE_DIR}/src/audio/audio_pack_service.h"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/laiue/audio"
        COMPONENT Engine)
endif()
if(TARGET laiue_mesher)
    install(FILES
        "${PROJECT_SOURCE_DIR}/src/graphics/mesh/chunk_mesher.h"
        "${PROJECT_SOURCE_DIR}/src/graphics/mesh/mesher_service.h"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/laiue/mesh"
        COMPONENT Engine)
endif()
if(TARGET laiue_render)
    install(FILES
            "${PROJECT_SOURCE_DIR}/src/graphics/render/chunk_geometry.h"
            "${PROJECT_SOURCE_DIR}/src/graphics/render/renderer.h"
            "${PROJECT_SOURCE_DIR}/src/graphics/render/graphics_service.h"
            "${PROJECT_SOURCE_DIR}/src/graphics/render/ui_quad.h"
            "${PROJECT_SOURCE_DIR}/src/graphics/render/shader_pack.h"
            "${PROJECT_SOURCE_DIR}/src/graphics/render/texture_pack.h"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/laiue/render"
        COMPONENT Engine)
endif()
if(TARGET laiue_input)
    install(FILES
            "${PROJECT_SOURCE_DIR}/src/graphics/input/input.h"
            "${PROJECT_SOURCE_DIR}/src/graphics/input/input_service.h"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/laiue/input"
        COMPONENT Engine)
endif()
if(TARGET laiue_scene)
    install(FILES
            "${PROJECT_SOURCE_DIR}/src/graphics/scene/camera.h"
            "${PROJECT_SOURCE_DIR}/src/graphics/scene/math.h"
            "${PROJECT_SOURCE_DIR}/src/graphics/scene/math_service.h"
            "${PROJECT_SOURCE_DIR}/src/graphics/scene/panorama.h"
            "${PROJECT_SOURCE_DIR}/src/graphics/scene/scene_service.h"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/laiue/scene"
        COMPONENT Engine)
endif()
if(TARGET laiue_voxel_render)
    install(FILES
        "${PROJECT_SOURCE_DIR}/src/graphics/voxel_render/chunk_streaming.h"
        "${PROJECT_SOURCE_DIR}/src/graphics/voxel_render/voxel_render_service.h"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/laiue/voxel_render"
        COMPONENT Engine)
endif()
if(TARGET laiue_voxel_raycast)
    install(FILES
        "${PROJECT_SOURCE_DIR}/src/simulation/voxel/raycast.h"
        "${PROJECT_SOURCE_DIR}/src/simulation/voxel/raycast_service.h"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/laiue/voxel"
        COMPONENT Engine)
endif()
if(TARGET laiue_ui)
    install(FILES
            "${PROJECT_SOURCE_DIR}/src/ui/ui.h"
            "${PROJECT_SOURCE_DIR}/src/ui/ui_service.h"
            "${PROJECT_SOURCE_DIR}/src/ui/ui_font.h"
            "${PROJECT_SOURCE_DIR}/src/ui/ui_format.h"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/laiue/ui"
        COMPONENT Engine)
endif()
if(TARGET laiue_window)
    install(FILES
            "${PROJECT_SOURCE_DIR}/src/platform/window.h"
            "${PROJECT_SOURCE_DIR}/src/platform/window_service.h"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/laiue/window"
        COMPONENT Engine)
endif()
if(LAIUE_BUILD_GRAPHICS)
    install(DIRECTORY "${PROJECT_SOURCE_DIR}/shaders/"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/laiue/shaders"
        COMPONENT Engine
        FILES_MATCHING PATTERN "*.hlsl")
endif()
install(FILES
        "${PROJECT_SOURCE_DIR}/LICENSE"
        "${PROJECT_SOURCE_DIR}/README.md"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/laiue"
    COMPONENT Engine)
install(DIRECTORY "${PROJECT_SOURCE_DIR}/docs/"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/laiue/docs"
    COMPONENT Engine
    FILES_MATCHING PATTERN "*.md")
install(DIRECTORY "${PROJECT_SOURCE_DIR}/sdk/native_module_template/"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/laiue/sdk/native_module_template"
    COMPONENT Engine
    FILES_MATCHING
        PATTERN "*.c"
        PATTERN "*.h"
        PATTERN "*.cmake"
        PATTERN "*.laiue"
        PATTERN "*.md")

configure_package_config_file(
    "${PROJECT_SOURCE_DIR}/cmake/laiue-config.cmake.in"
    "${PROJECT_BINARY_DIR}/laiue-config.cmake"
    INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/laiue")
write_basic_package_version_file(
    "${PROJECT_BINARY_DIR}/laiue-config-version.cmake"
    VERSION "${PROJECT_VERSION}"
    COMPATIBILITY SameMajorVersion)
install(EXPORT laiueTargets
    FILE laiueTargets.cmake
    NAMESPACE laiue::
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/laiue"
    COMPONENT Engine)
install(FILES
        "${PROJECT_BINARY_DIR}/laiue-config.cmake"
        "${PROJECT_BINARY_DIR}/laiue-config-version.cmake"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/laiue"
    COMPONENT Engine)

set(_laiue_bundle_directory
    "${CMAKE_BINARY_DIR}/bundles/engine/$<CONFIG>")
set(_laiue_bundle_strip_argument)
if(NOT WIN32)
    set(_laiue_bundle_strip_argument "$<$<CONFIG:Release>:--strip>")
endif()
add_custom_target(laiue_engine_bundle
    COMMAND "${CMAKE_COMMAND}" -E remove_directory
        "${_laiue_bundle_directory}"
    COMMAND "${CMAKE_COMMAND}" --install "${CMAKE_BINARY_DIR}"
        --config "$<CONFIG>"
        --prefix "${_laiue_bundle_directory}"
        --component Engine
        ${_laiue_bundle_strip_argument}
    DEPENDS ${LAIUE_RUNTIME_TARGETS} laiue_bootstrap laiue_platform_support
    COMMAND_EXPAND_LISTS
    COMMENT "Staging the laiue engine SDK"
    VERBATIM)
add_custom_target(distribution DEPENDS laiue_engine_bundle)

set(CPACK_PACKAGE_NAME "laiue-engine")
set(CPACK_PACKAGE_VENDOR "landiunload")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_FILE_NAME
    "laiue-engine-${PROJECT_VERSION}-${CMAKE_SYSTEM_NAME}-${LAIUE_PACKAGE_ARCHITECTURE}")
set(CPACK_COMPONENTS_ALL Engine)
if(WIN32)
    set(CPACK_GENERATOR ZIP)
else()
    set(CPACK_GENERATOR TGZ)
endif()
    include(CPack)
endif()

# Инструменты подготовки содержимого — программы для машины
# разработчика, а не часть SDK. При кросс-компиляции их собирать
# бессмысленно: запускать их предстоит там же, где лежат исходные файлы.
if(LAIUE_PLATFORM_EXTERNAL OR CMAKE_CROSSCOMPILING)
    set(_laiue_tools_default OFF)
else()
    set(_laiue_tools_default ON)
endif()
option(LAIUE_BUILD_TOOLS "Build the offline content tools"
    ${_laiue_tools_default})

option(LAIUE_BUILD_EXAMPLES "Build the public SDK examples" OFF)
if(LAIUE_BUILD_EXAMPLES)
    add_subdirectory(examples/walk)
endif()

if(LAIUE_BUILD_TOOLS)
    add_subdirectory(tools/soundc)
    add_subdirectory(tools/texc)
endif()

option(LAIUE_BUILD_BENCHMARKS "Build manual performance benchmarks" OFF)
if(LAIUE_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()

include(CTest)
if(BUILD_TESTING)
    find_package(Git REQUIRED)
    add_subdirectory(tests)
    if(TARGET laiue_walk)
        add_test(NAME laiue.example.walk COMMAND laiue_walk)
    endif()
    add_test(NAME laiue.architecture.boundaries
        COMMAND "${CMAKE_COMMAND}"
            "-DSOURCE_ROOT=${PROJECT_SOURCE_DIR}/src"
            -P "${PROJECT_SOURCE_DIR}/tools/check_architecture.cmake")
    add_test(NAME laiue.build.source_guard_regression
        COMMAND "${CMAKE_COMMAND}"
            "-DVERIFY_SCRIPT=${PROJECT_SOURCE_DIR}/tools/verify_source_tree.cmake"
            "-DTEST_ROOT=${PROJECT_BINARY_DIR}/source-guard-test"
            "-DGIT_EXECUTABLE=${GIT_EXECUTABLE}"
            -P "${PROJECT_SOURCE_DIR}/tests/verify_source_tree_test.cmake")
    if(TARGET laiue_static_core_link_smoke)
        add_test(NAME laiue.build.static_core_link
            COMMAND laiue_static_core_link_smoke)
    endif()
    if(LAIUE_ENABLE_SDK_INSTALL_RESOLVED)
        add_test(NAME laiue.install.consumer
            COMMAND "${CMAKE_COMMAND}"
                "-DENGINE_BINARY_DIR=${PROJECT_BINARY_DIR}"
                "-DCONSUMER_SOURCE_DIR=${PROJECT_SOURCE_DIR}/tests/consumer"
                "-DTEST_ROOT=${PROJECT_BINARY_DIR}/installed-consumer-$<CONFIG>"
                "-DCONFIGURATION=$<CONFIG>"
                "-DENGINE_GENERATOR=${CMAKE_GENERATOR}"
                "-DENGINE_GENERATOR_PLATFORM=${CMAKE_GENERATOR_PLATFORM}"
                "-DENGINE_GENERATOR_TOOLSET=${CMAKE_GENERATOR_TOOLSET}"
                "-DENGINE_MULTI_CONFIG=${LAIUE_IS_MULTI_CONFIG}"
                "-DHOST_WIN32=${WIN32}"
                "-DENGINE_SANITIZERS=${LAIUE_ENABLE_SANITIZERS}"
                -P "${PROJECT_SOURCE_DIR}/tests/run_installed_consumer_test.cmake")
        set_tests_properties(laiue.install.consumer PROPERTIES
            RUN_SERIAL TRUE
            TIMEOUT 120)
    endif()
endif()
