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
    DESCRIPTION "C17 voxel engine libraries with infinite coordinates"
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
    "Build the window/input/audio/mesher/render/scene/UI layer"
    ${_laiue_graphics_default})

# Набор графических модулей задаёт бэкенд, а не платформа. D3D12 несёт
# весь слой; Vulkan на первом этапе — мешер и offscreen-рендер, потому
# что окно, ввод, звук, сцена и шрифты интерфейса всё ещё написаны прямо
# на Win32/GDI. Звук переносим целиком: микшер платформы не знает, а
# вывод у него свой на каждой. Список выводится в конфигурации, чтобы
# неполнота профиля была видна, а не подразумевалась.
if(LAIUE_RENDER_BACKEND_RESOLVED STREQUAL "D3D12")
    set(LAIUE_GRAPHICS_MODULE_DIRECTORIES input audio mesh render scene ui)
else()
    set(LAIUE_GRAPHICS_MODULE_DIRECTORIES audio mesh render scene)
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

add_subdirectory(src/math)
add_subdirectory(src/media)
add_subdirectory(src/platform)
add_subdirectory(src/world)
add_subdirectory(src/physics)
add_subdirectory(src/content)
add_subdirectory(src/mod)

if(LAIUE_BUILD_GRAPHICS)
    foreach(graphics_directory IN LISTS LAIUE_GRAPHICS_MODULE_DIRECTORIES)
        add_subdirectory("src/${graphics_directory}")
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
        "laiue_world,laiue_physics,laiue_content,laiue_mod>")
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
    EXPORT laiueTargets
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT Engine
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT Engine
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT Engine
    INCLUDES DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/laiue"
)

install(FILES "${PROJECT_SOURCE_DIR}/src/api.h"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/laiue"
    COMPONENT Engine)
install(FILES
        "${PROJECT_SOURCE_DIR}/src/world/world.h"
        "${PROJECT_SOURCE_DIR}/src/world/infinite_coord.h"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/laiue/world"
    COMPONENT Engine)
install(FILES "${PROJECT_SOURCE_DIR}/src/physics/voxel_body.h"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/laiue/physics"
    COMPONENT Engine)
install(FILES
        "${PROJECT_SOURCE_DIR}/src/content/content_catalog.h"
        "${PROJECT_SOURCE_DIR}/src/content/content_format.h"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/laiue/content"
    COMPONENT Engine)
install(FILES
        "${PROJECT_SOURCE_DIR}/src/mod/mod.h"
        "${PROJECT_SOURCE_DIR}/src/mod/mod_api.h"
        "${PROJECT_SOURCE_DIR}/src/mod/mod_host.h"
        "${PROJECT_SOURCE_DIR}/src/mod/mod_manifest.h"
        "${PROJECT_SOURCE_DIR}/src/mod/mod_types.h"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/laiue/mod"
    COMPONENT Engine)
# Заголовки ставятся по собранным целям, а не по одному флагу графики:
# набор графических модулей зависит от бэкенда, и SDK не должен обещать
# заголовок модуля, которого в сборке нет.
if(TARGET laiue_window)
    install(FILES
            "${PROJECT_SOURCE_DIR}/src/platform/window.h"
            "${PROJECT_SOURCE_DIR}/src/platform/time.h"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/laiue/platform"
        COMPONENT Engine)
endif()
if(TARGET laiue_input)
    install(FILES "${PROJECT_SOURCE_DIR}/src/input/input.h"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/laiue/input"
        COMPONENT Engine)
endif()
if(TARGET laiue_audio)
    install(FILES
            "${PROJECT_SOURCE_DIR}/src/audio/audio.h"
            "${PROJECT_SOURCE_DIR}/src/audio/audio_pack.h"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/laiue/audio"
        COMPONENT Engine)
endif()
if(TARGET laiue_mesher)
    install(FILES "${PROJECT_SOURCE_DIR}/src/mesh/chunk_mesher.h"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/laiue/mesh"
        COMPONENT Engine)
endif()
if(TARGET laiue_render)
    install(FILES
            "${PROJECT_SOURCE_DIR}/src/render/chunk_geometry.h"
            "${PROJECT_SOURCE_DIR}/src/render/renderer.h"
            "${PROJECT_SOURCE_DIR}/src/render/shader_pack.h"
            "${PROJECT_SOURCE_DIR}/src/render/texture_pack.h"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/laiue/render"
        COMPONENT Engine)
endif()
if(TARGET laiue_scene)
    install(FILES
            "${PROJECT_SOURCE_DIR}/src/scene/camera.h"
            "${PROJECT_SOURCE_DIR}/src/scene/chunk_streaming.h"
            "${PROJECT_SOURCE_DIR}/src/scene/math.h"
            "${PROJECT_SOURCE_DIR}/src/scene/panorama.h"
            "${PROJECT_SOURCE_DIR}/src/scene/voxel_raycast.h"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/laiue/scene"
        COMPONENT Engine)
endif()
if(TARGET laiue_ui)
    install(FILES
            "${PROJECT_SOURCE_DIR}/src/ui/ui.h"
            "${PROJECT_SOURCE_DIR}/src/ui/ui_font.h"
            "${PROJECT_SOURCE_DIR}/src/ui/ui_format.h"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/laiue/ui"
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
    DEPENDS ${LAIUE_RUNTIME_TARGETS}
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
if(LAIUE_BUILD_TOOLS)
    add_subdirectory(tools/soundc)
    add_subdirectory(tools/texc)
endif()

option(LAIUE_BUILD_BENCHMARKS "Build manual performance benchmarks" OFF)
if(LAIUE_BUILD_BENCHMARKS)
    if(NOT TARGET laiue_mesher OR NOT TARGET laiue_audio)
        message(FATAL_ERROR
            "Бенчмарк измеряет мешер и микшер; включите LAIUE_BUILD_GRAPHICS")
    endif()
    add_subdirectory(benchmarks)
endif()

include(CTest)
if(BUILD_TESTING)
    find_package(Git REQUIRED)
    add_subdirectory(tests)
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
