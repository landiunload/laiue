cmake_minimum_required(VERSION 4.2...4.4)

# Отладочная информация встраивается в объектный файл (/Z7), а не пишется в
# общий PDB: no-CRT сборке не нужен отдельный файл, а компиляция не
# сериализуется на записи в него. /RTC требует CRT, поэтому runtime checks
# выключены.
set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<$<CONFIG:Debug,RelWithDebInfo>:Embedded>")
set(CMAKE_MSVC_RUNTIME_CHECKS "")

project(laiue
    VERSION 0.1.0
    DESCRIPTION "Modular C17 engine for any application"
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

# Audio mixer, platform output, and UI are technology modules in their own
# right. UI does not require a renderer at compile time, but its runtime
# service graph requires laiue.graphics; headless profiles can still build it
# and omit it from the selected module profile.
option(LAIUE_BUILD_AUDIO "Build the audio mixer and deterministic offscreen output" ON)
option(LAIUE_BUILD_AUDIO_OUTPUT "Build the optional WASAPI/ALSA audio output provider" ON)
option(LAIUE_BUILD_AUDIO_PACK "Build the optional audio pack/decoder module" ON)
option(LAIUE_BUILD_UI "Build the backend-neutral UI module" ON)
option(LAIUE_BUILD_MODEL "Build the model pack module (OBJ and .lo)" ON)
option(LAIUE_BUILD_ANIMATION "Build the animation pack module (.lk)" ON)
option(LAIUE_BUILD_MESH_WORLD "Build the infinite mesh world module" ON)

# Набор графического backend-а не выбирает оконный и вводный provider.
# Windows-слой одинаково доступен для D3D12 и Vulkan; сам Vulkan при этом
# также умеет headless/offscreen. На Linux окно и ввод собираются поверх
# X11, если он найден; на macOS и внешних платформах их даёт adapter.
if(LAIUE_PLATFORM_WINDOWS)
    set(LAIUE_GRAPHICS_MODULE_DIRECTORIES input mesh profile render scene voxel_render)
else()
    set(LAIUE_GRAPHICS_MODULE_DIRECTORIES mesh profile render scene voxel_render)
    if(LAIUE_BUILD_GRAPHICS AND LAIUE_PLATFORM_POSIX AND NOT APPLE)
        find_package(X11 QUIET)
        if(X11_FOUND)
            list(APPEND LAIUE_GRAPHICS_MODULE_DIRECTORIES input)
        endif()
    elseif(LAIUE_BUILD_GRAPHICS AND ANDROID)
        # Android даёт окно и ввод сам: NativeActivity, ANativeWindow и
        # AInputEvent.
        list(APPEND LAIUE_GRAPHICS_MODULE_DIRECTORIES input)
    endif()
endif()

include(LaiueToolchain)
include(LaiueModule)
include(LaiueShader)
include(LaiueArchitecture)
include(LaiueStandaloneExecutable)
if(ANDROID)
    include(LaiueAndroid)
endif()
include(LaiueProgram)

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
add_subdirectory(src/persist)
option(LAIUE_BUILD_DIAGNOSTICS "Build debug-only diagnostics such as the frame monitor" ON)
if(LAIUE_BUILD_DIAGNOSTICS)
    add_subdirectory(src/diagnostics/frame_monitor)
endif()
add_subdirectory(src/simulation/world)
add_subdirectory(src/simulation/physics)
add_subdirectory(src/simulation/navigation)
add_subdirectory(src/assets/content)
add_subdirectory(src/simulation/character)
add_subdirectory(src/simulation/voxel)
add_subdirectory(src/modding/mod)
if(LAIUE_BUILD_AUDIO OR LAIUE_BUILD_AUDIO_OUTPUT OR LAIUE_BUILD_AUDIO_PACK)
    add_subdirectory(src/audio)
endif()
if(LAIUE_BUILD_UI)
    add_subdirectory(src/ui)
endif()
if(LAIUE_BUILD_MODEL)
    add_subdirectory(src/assets/model)
endif()
if(LAIUE_BUILD_ANIMATION)
    add_subdirectory(src/assets/animation)
endif()
if(LAIUE_BUILD_MESH_WORLD)
    add_subdirectory(src/simulation/mesh_world)
    add_subdirectory(src/graphics/mesh_world_render)
endif()

# Minimal application bootstrap. It contains only the standalone module
# resolver and the platform boundary; it deliberately does not link the
# legacy aggregate `laiue_engine` or any graphics/physics/voxel library. An
# application can therefore embed this static archive and choose an explicit
# module graph at runtime. The existing `laiue::mod` target remains the
# compatibility aggregate for old native mods.
add_library(laiue_bootstrap STATIC
    src/modding/mod/application.c
    src/modding/mod/application.h
    src/modding/mod/module_api.h
    src/modding/mod/module_artifact.c
    src/modding/mod/module_manifest.c
    src/modding/mod/module_manifest.h
    src/modding/mod/module_profile.c
    src/modding/mod/module_profile.h
    src/modding/mod/module_host.c
    src/modding/mod/module_host.h
    src/modding/mod/mod_internal.c
    src/modding/mod/mod_internal.h
    src/modding/mod/mod_types.h)
add_library(laiue::bootstrap ALIAS laiue_bootstrap)
target_include_directories(laiue_bootstrap
    PUBLIC ${LAIUE_PUBLIC_INCLUDE_DIRECTORIES})
# The engine's compile rules apply to the archive itself, but neither its
# no-CRT link options nor its no-CRT memcpy/memset objects may reach the
# application that links it: that application chooses its own C runtime.
# Engine executables without a CRT get those objects from laiue_common.
target_link_libraries(laiue_bootstrap
    PRIVATE
        $<COMPILE_ONLY:laiue::build_options>
        laiue::platform_support)
# Public, as in the SDK package: its consumers call these functions directly
# from the static archive, not through a DLL import.
target_compile_definitions(laiue_bootstrap PUBLIC LAIUE_STATIC=1)
laiue_define_module_artifact_names(laiue_bootstrap)
# An application links this archive with its own compiler, so it carries
# ordinary machine code rather than LTO objects of the engine's compiler.
set_target_properties(laiue_bootstrap PROPERTIES
    EXPORT_NAME bootstrap
    POSITION_INDEPENDENT_CODE ON
    LAIUE_PORTABLE_ARCHIVE ON)

# Public contracts for optional technology providers. These are intentionally
# header-only interface targets: applications can compile a provider without
# pulling the voxel, renderer, or physics implementation into the process.
foreach(api IN ITEMS graphics voxel character)
    add_library(laiue_${api}_api INTERFACE)
    add_library(laiue::${api}_api ALIAS laiue_${api}_api)
    target_include_directories(laiue_${api}_api INTERFACE ${LAIUE_PUBLIC_INCLUDE_DIRECTORIES})
    set_target_properties(laiue_${api}_api PROPERTIES EXPORT_NAME ${api}_api)
endforeach()

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
    get_property(_laiue_static_core_targets GLOBAL PROPERTY LAIUE_RUNTIME_CORE_TARGETS)
    list(JOIN _laiue_static_core_targets "," _laiue_static_core_names)
    set(_laiue_static_core_link "$<LINK_LIBRARY:WHOLE_ARCHIVE,${_laiue_static_core_names}>")
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
            XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "dev.laiue.core-link-smoke"
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

# Диагностические модули — только в отладочных конфигурациях SDK и вне
# экспорта пакета: приложение загружает их по имени, а в релизе их нет.
get_property(LAIUE_RUNTIME_DIAGNOSTICS_TARGETS GLOBAL PROPERTY LAIUE_RUNTIME_DIAGNOSTICS_TARGETS)
if(LAIUE_RUNTIME_DIAGNOSTICS_TARGETS)
    install(TARGETS ${LAIUE_RUNTIME_DIAGNOSTICS_TARGETS}
        CONFIGURATIONS Debug RelWithDebInfo
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT Diagnostics
        LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT Diagnostics
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT Diagnostics)
endif()

# The bootstrap archive intentionally keeps its private platform/no-CRT link
# implementation out of the public aggregate export. It is still staged with
# the SDK and can be linked directly by applications built in the same
# toolchain; the package export for the legacy aggregate remains unchanged.
install(TARGETS laiue_bootstrap laiue_platform_support
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT Engine
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT Engine
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT Engine)

# Public headers follow the built targets, not one graphics flag: the module
# set depends on the backend, and the SDK must not promise a header of a
# module that is not in the build.
function(laiue_install_headers target directory)
    if(TARGET ${target})
        list(TRANSFORM ARGN PREPEND "${PROJECT_SOURCE_DIR}/src/")
        install(FILES ${ARGN}
            DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/laiue/${directory}"
            COMPONENT Engine)
    endif()
endfunction()

install(FILES "${PROJECT_SOURCE_DIR}/src/api.h"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/laiue"
    COMPONENT Engine)
laiue_install_headers(laiue_numeric numeric
    numeric/infinite_coord.h numeric/numeric_service.h)
laiue_install_headers(laiue_world world
    simulation/world/world.h simulation/world/world_service.h
    simulation/world/numeric_provider.h)
laiue_install_headers(laiue_physics physics
    simulation/physics/rigid_body.h simulation/physics/compound_shape.h
    simulation/physics/rigid_broadphase.h simulation/physics/ragdoll.h
    simulation/physics/voxel_body.h simulation/physics/physics_service.h
    simulation/physics/numeric_provider.h)
laiue_install_headers(laiue_task task jobs/task/task_pool.h jobs/task/task_service.h)
laiue_install_headers(laiue_persist persist persist/persist.h persist/persist_service.h)
# Заголовки диагностики ставятся всегда: без LAIUE_DIAGNOSTICS их макросы пусты.
laiue_install_headers(laiue_frame_monitor frame_monitor
    diagnostics/frame_monitor/frame_monitor.h diagnostics/frame_monitor/frame_monitor_service.h)
laiue_install_headers(laiue_navigation navigation
    simulation/navigation/navigation.h simulation/navigation/navigation_service.h)
laiue_install_headers(laiue_content content
    assets/content/content_catalog.h assets/content/content_format.h
    assets/content/content_service.h)
laiue_install_headers(laiue_mod mod
    modding/mod/application.h
    modding/mod/mod.h modding/mod/mod_api.h modding/mod/mod_host.h
    modding/mod/mod_manifest.h modding/mod/mod_types.h modding/mod/module_api.h
    modding/mod/module_service.h modding/mod/module_manifest.h
    modding/mod/module_profile.h modding/mod/module_host.h)
laiue_install_headers(laiue_graphics_api graphics
    graphics/graphics_api.h graphics/graphics_device_service.h
    graphics/graphics_device_v2.h)
laiue_install_headers(laiue_voxel_api voxel
    simulation/voxel/voxel_api.h simulation/voxel/voxel_service.h)
laiue_install_headers(laiue_character_api character
    simulation/character/character_api.h simulation/character/character_service.h)
laiue_install_headers(laiue_model model assets/model/model_service.h)
laiue_install_headers(laiue_animation animation assets/animation/animation_service.h)
laiue_install_headers(laiue_mesh_world mesh_world simulation/mesh_world/mesh_world_service.h)
laiue_install_headers(laiue_mesh_world_render mesh_world_render
    graphics/mesh_world_render/mesh_world_render_service.h)
# The window headers keep their source directory: window_service.h includes
# "platform/window.h".
laiue_install_headers(laiue_window platform
    platform/time.h platform/window.h platform/window_service.h)
laiue_install_headers(laiue_audio audio
    audio/audio.h audio/audio_stream.h audio/audio_offscreen.h audio/audio_service.h)
laiue_install_headers(laiue_audio_output audio audio/audio_output_service.h)
laiue_install_headers(laiue_audio_pack audio audio/audio_pack.h audio/audio_pack_service.h)
laiue_install_headers(laiue_mesher mesh
    graphics/mesh/chunk_mesher.h graphics/mesh/mesher_service.h graphics/mesh/surface_mesher.h)
laiue_install_headers(laiue_render render
    graphics/render/chunk_geometry.h graphics/render/renderer.h
    graphics/render/graphics_service.h graphics/render/content_provider.h
    graphics/render/ui_quad.h graphics/render/shader_pack.h
    graphics/render/texture_pack.h)
laiue_install_headers(laiue_input input graphics/input/input.h graphics/input/input_service.h)
laiue_install_headers(laiue_scene scene
    graphics/scene/camera.h graphics/scene/math.h graphics/scene/math_service.h
    graphics/scene/panorama.h graphics/scene/scene_service.h)
laiue_install_headers(laiue_voxel_render voxel_render
    graphics/voxel_render/chunk_streaming.h graphics/voxel_render/voxel_render_service.h)
laiue_install_headers(laiue_voxel_raycast voxel
    simulation/voxel/raycast.h simulation/voxel/raycast_service.h)
laiue_install_headers(laiue_ui ui ui/ui.h ui/ui_service.h ui/ui_font.h ui/ui_format.h)
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
        PATTERN "CMakeLists.txt"
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

# Internal scenario/measurement support is shared by the walk example, offline
# frame validator and tests. It is not a module or part of the installed SDK.
add_library(laiue_walk_scenario_support STATIC examples/walk/walk_scenario.c)
target_link_libraries(laiue_walk_scenario_support PRIVATE laiue::build_options)
set_target_properties(laiue_walk_scenario_support PROPERTIES POSITION_INDEPENDENT_CODE ON)
option(LAIUE_BUILD_ANDROID_WALK
    "Build the NativeActivity/Vulkan Android walk client (requires Android NDK)"
    OFF)
# Private game code is compiled once and reused by shells, tests and benchmarks.
# Keep the archive out of the default build when none of those consumers is used.
add_library(laiue_walk_common STATIC EXCLUDE_FROM_ALL
    examples/walk/walk_runtime.c examples/walk/walk_runtime.h
    examples/walk/walk_gameplay.c examples/walk/walk_gameplay.h
    examples/walk/walk_humanoid.c examples/walk/walk_humanoid.h
    examples/walk/walk_physics.c examples/walk/walk_physics.h
    examples/walk/walk_visuals.c examples/walk/walk_visuals.h
    examples/walk/walk_world.c examples/walk/walk_world.h)
target_include_directories(laiue_walk_common PRIVATE
    "${PROJECT_SOURCE_DIR}/src"
    "${PROJECT_SOURCE_DIR}/src/assets"
    "${PROJECT_SOURCE_DIR}/src/simulation")
target_link_libraries(laiue_walk_common PRIVATE
    laiue::build_options laiue::math_support
    laiue_walk_scenario_support laiue_media_support)
set_target_properties(laiue_walk_common PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(ANDROID OR LAIUE_MODULE_LIBRARY_TYPE_RESOLVED STREQUAL "STATIC")
    target_compile_definitions(laiue_walk_common PRIVATE LAIUE_STATIC=1)
endif()
if(LAIUE_BUILD_EXAMPLES)
    add_subdirectory(examples/walk)
    add_subdirectory(examples/mesh_world)
    add_subdirectory(examples/animation)
    # Программа на модулях по имени: окно, ввод и графика — в самой сборке.
    if(LAIUE_MODULE_LIBRARY_TYPE_RESOLVED STREQUAL "SHARED" AND TARGET laiue_window AND
       TARGET laiue_input)
        add_subdirectory(examples/viewer)
    endif()
endif()

if(LAIUE_BUILD_TOOLS)
    add_executable(laiue_frame_check tools/frame_check.c)
    target_include_directories(laiue_frame_check PRIVATE
        "${PROJECT_SOURCE_DIR}/src"
        "${PROJECT_SOURCE_DIR}/src/assets"
        "${PROJECT_SOURCE_DIR}/examples/walk")
    target_link_libraries(laiue_frame_check PRIVATE
        laiue::media_support laiue_walk_scenario_support)
    laiue_configure_standalone_executable(laiue_frame_check FrameCheckEntryPoint)
    if(WIN32)
        target_link_libraries(laiue_frame_check PRIVATE shell32)
    endif()
    add_subdirectory(tools/soundc)
    add_subdirectory(tools/modelc)
    add_subdirectory(tools/texc)
endif()

option(LAIUE_BUILD_BENCHMARKS "Build manual performance benchmarks" OFF)
if(LAIUE_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()

include(CTest)
# Фаззеры регистрируют в CTest короткие прогоны поверх семян.
if(LAIUE_BUILD_FUZZERS)
    add_subdirectory(tests/fuzz)
endif()
if(BUILD_TESTING)
    find_package(Git REQUIRED)
    add_subdirectory(tests)
    if(TARGET laiue_walk)
        add_test(NAME laiue.example.walk COMMAND laiue_walk --headless)
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
    if(LAIUE_PLATFORM_WINDOWS)
        add_test(NAME laiue.build.bootstrap_without_runtime
            COMMAND "${CMAKE_COMMAND}"
                "-DARCHIVER=${CMAKE_AR}"
                "-DARCHIVE=$<TARGET_FILE:laiue_bootstrap>"
                "-DFORBIDDEN=$<JOIN:$<TARGET_OBJECTS:laiue_runtime>,|>"
                -P "${PROJECT_SOURCE_DIR}/tools/check_archive_members.cmake")
    endif()
    if(LAIUE_ENABLE_SDK_INSTALL_RESOLVED)
        # Релизный SDK и релизные бинарники без тестов, бенчмарков,
        # отладочной информации и диагностики.
        add_test(NAME laiue.build.release_hygiene
            CONFIGURATIONS Release
            COMMAND "${CMAKE_COMMAND}"
                "-DENGINE_BINARY_DIR=${PROJECT_BINARY_DIR}"
                "-DRELEASE_RUNTIME_DIR=$<TARGET_FILE_DIR:laiue_persist>"
                "-DTEST_ROOT=${PROJECT_BINARY_DIR}/release-hygiene"
                -P "${PROJECT_SOURCE_DIR}/tools/check_release_hygiene.cmake")
        set_tests_properties(laiue.build.release_hygiene PROPERTIES RUN_SERIAL TRUE TIMEOUT 120)
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
                "-DENGINE_SANITIZERS=$<OR:$<BOOL:${LAIUE_ENABLE_SANITIZERS}>,$<BOOL:${LAIUE_BUILD_FUZZERS}>>"
                "-DENGINE_C_COMPILER=${CMAKE_C_COMPILER}"
                "-DENGINE_VERSION=${PROJECT_VERSION}"
                -P "${PROJECT_SOURCE_DIR}/tests/run_installed_consumer_test.cmake")
        set_tests_properties(laiue.install.consumer PROPERTIES
            RUN_SERIAL TRUE
            TIMEOUT 120)
    endif()
endif()
