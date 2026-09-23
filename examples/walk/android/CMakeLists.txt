if(NOT ANDROID)
    message(FATAL_ERROR "examples/walk/android requires an Android toolchain")
endif()

set(_laiue_android_ndk "${CMAKE_ANDROID_NDK}")
if(_laiue_android_ndk STREQUAL "")
    set(_laiue_android_ndk "${ANDROID_NDK}")
endif()
set(_laiue_android_native_app_glue
    "${_laiue_android_ndk}/sources/android/native_app_glue/android_native_app_glue.c")
if(NOT EXISTS "${_laiue_android_native_app_glue}")
    message(FATAL_ERROR
        "Android NDK native_app_glue.c was not found: ${_laiue_android_native_app_glue}")
endif()

add_library(laiue_walk_android SHARED
    android_walk.c
    ../walk.c
    "${_laiue_android_native_app_glue}")
target_compile_definitions(laiue_walk_android PRIVATE
    LAIUE_STATIC=1
    LAIUE_ANDROID_WALK=1
    LAIUE_WALK_RUNTIME_ONLY=1)
target_include_directories(laiue_walk_android PRIVATE
    "${PROJECT_SOURCE_DIR}/src"
    "${PROJECT_SOURCE_DIR}/src/simulation"
    "${PROJECT_SOURCE_DIR}/src/graphics"
    "${PROJECT_SOURCE_DIR}/src/modding"
    "${CMAKE_CURRENT_SOURCE_DIR}/.."
    "${_laiue_android_ndk}/sources/android/native_app_glue")
target_link_libraries(laiue_walk_android PRIVATE
    laiue_bootstrap
    laiue_character
    laiue_render
    android
    log
    "${Vulkan_LIBRARY}")
if(LAIUE_ANDROID_WALK_WITH_VOXEL)
    target_link_libraries(laiue_walk_android PRIVATE laiue_voxel)
    target_compile_definitions(laiue_walk_android PRIVATE LAIUE_ANDROID_WALK_WITH_VOXEL=1)
endif()
target_link_options(laiue_walk_android PRIVATE
    "-uANativeActivity_onCreate")
set_target_properties(laiue_walk_android PROPERTIES
    OUTPUT_NAME "laiue_walk"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/android/$<CONFIG>"
    POSITION_INDEPENDENT_CODE ON)

# The Java/manifest wrapper is deliberately kept outside the engine target:
# platform packagers can put this .so into their own application while the
# NativeActivity ABI and module selection stay identical.
add_custom_target(laiue_walk_android_manifest ALL
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/AndroidManifest.xml"
        "${CMAKE_BINARY_DIR}/android/$<CONFIG>/AndroidManifest.xml"
    DEPENDS laiue_walk_android
    VERBATIM)

# Build-tools are platform tooling, not a link-time dependency of the native
# client.  If an SDK root is available, discover the newest installed
# version, while keeping every path overridable for reproducible CI.
set(_laiue_android_sdk "$ENV{ANDROID_SDK_ROOT}")
if(_laiue_android_sdk STREQUAL "")
    set(_laiue_android_sdk "$ENV{ANDROID_HOME}")
endif()
if(_laiue_android_sdk STREQUAL "" AND NOT _laiue_android_ndk STREQUAL "")
    get_filename_component(_laiue_android_sdk "${_laiue_android_ndk}/../.." ABSOLUTE)
endif()
if(NOT _laiue_android_sdk STREQUAL "")
    file(GLOB _laiue_android_build_tools_dirs LIST_DIRECTORIES true
        "${_laiue_android_sdk}/build-tools/*")
    list(SORT _laiue_android_build_tools_dirs COMPARE NATURAL ORDER DESCENDING)
    list(LENGTH _laiue_android_build_tools_dirs _laiue_android_build_tools_count)
    if(_laiue_android_build_tools_count GREATER 0)
        list(GET _laiue_android_build_tools_dirs 0 _laiue_android_build_tools)
    endif()
    file(GLOB _laiue_android_platform_jars LIST_DIRECTORIES false
        "${_laiue_android_sdk}/platforms/android-*/android.jar")
    list(SORT _laiue_android_platform_jars COMPARE NATURAL ORDER DESCENDING)
    list(LENGTH _laiue_android_platform_jars _laiue_android_platform_count)
    if(_laiue_android_platform_count GREATER 0)
        list(GET _laiue_android_platform_jars 0 _laiue_android_framework_default)
    endif()
endif()

set(LAIUE_ANDROID_AAPT2 "" CACHE FILEPATH "aapt2 used to package the Android walk APK")
set(LAIUE_ANDROID_ZIPALIGN "" CACHE FILEPATH "zipalign used to package the Android walk APK")
set(LAIUE_ANDROID_APKSIGNER "" CACHE FILEPATH "apksigner used to sign the Android walk APK")
set(LAIUE_ANDROID_FRAMEWORK_JAR "" CACHE FILEPATH "android.jar for aapt2")
set(LAIUE_ANDROID_KEYSTORE "" CACHE FILEPATH "debug/release keystore for the Android walk APK")
set(LAIUE_ANDROID_KEYSTORE_PASSWORD "android" CACHE STRING
    "Keystore password used by the walk APK packaging target")

if(LAIUE_ANDROID_AAPT2 STREQUAL "" OR LAIUE_ANDROID_ZIPALIGN STREQUAL "")
    foreach(_laiue_android_tool_suffix IN ITEMS
            "${CMAKE_EXECUTABLE_SUFFIX}" ".exe" "")
        if(LAIUE_ANDROID_AAPT2 STREQUAL "" AND
           EXISTS "${_laiue_android_build_tools}/aapt2${_laiue_android_tool_suffix}")
            set(LAIUE_ANDROID_AAPT2
                "${_laiue_android_build_tools}/aapt2${_laiue_android_tool_suffix}"
                CACHE FILEPATH "aapt2 used to package the Android walk APK" FORCE)
        endif()
        if(LAIUE_ANDROID_ZIPALIGN STREQUAL "" AND
           EXISTS "${_laiue_android_build_tools}/zipalign${_laiue_android_tool_suffix}")
            set(LAIUE_ANDROID_ZIPALIGN
                "${_laiue_android_build_tools}/zipalign${_laiue_android_tool_suffix}"
                CACHE FILEPATH "zipalign used to package the Android walk APK" FORCE)
        endif()
    endforeach()
endif()
if(LAIUE_ANDROID_APKSIGNER STREQUAL "")
    if(EXISTS "${_laiue_android_build_tools}/apksigner${CMAKE_EXECUTABLE_SUFFIX}")
        set(_laiue_android_apksigner_default
            "${_laiue_android_build_tools}/apksigner${CMAKE_EXECUTABLE_SUFFIX}")
    elseif(EXISTS "${_laiue_android_build_tools}/apksigner.bat")
        set(_laiue_android_apksigner_default
            "${_laiue_android_build_tools}/apksigner.bat")
    endif()
    if(NOT _laiue_android_apksigner_default STREQUAL "")
        set(LAIUE_ANDROID_APKSIGNER "${_laiue_android_apksigner_default}"
            CACHE FILEPATH "apksigner used to sign the Android walk APK" FORCE)
    endif()
endif()
if(LAIUE_ANDROID_FRAMEWORK_JAR STREQUAL "" AND
   EXISTS "${_laiue_android_framework_default}")
    set(LAIUE_ANDROID_FRAMEWORK_JAR "${_laiue_android_framework_default}"
        CACHE FILEPATH "android.jar for aapt2" FORCE)
endif()

if(LAIUE_ANDROID_AAPT2 AND LAIUE_ANDROID_ZIPALIGN AND LAIUE_ANDROID_APKSIGNER AND
   LAIUE_ANDROID_FRAMEWORK_JAR AND LAIUE_ANDROID_KEYSTORE)
    find_program(_laiue_powershell NAMES pwsh powershell REQUIRED)
    add_custom_target(laiue_walk_android_apk
        COMMAND "${_laiue_powershell}" -NoProfile -ExecutionPolicy Bypass -File
            "${CMAKE_CURRENT_SOURCE_DIR}/package_apk.ps1"
            -Aapt2 "${LAIUE_ANDROID_AAPT2}"
            -Zipalign "${LAIUE_ANDROID_ZIPALIGN}"
            -Apksigner "${LAIUE_ANDROID_APKSIGNER}"
            -FrameworkJar "${LAIUE_ANDROID_FRAMEWORK_JAR}"
            -Manifest "${CMAKE_CURRENT_SOURCE_DIR}/AndroidManifest.xml"
            -NativeLib "$<TARGET_FILE:laiue_walk_android>"
            -Abi "${ANDROID_ABI}"
            -Keystore "${LAIUE_ANDROID_KEYSTORE}"
            -KeystorePassword "${LAIUE_ANDROID_KEYSTORE_PASSWORD}"
            -OutputDirectory "${CMAKE_BINARY_DIR}/android/$<CONFIG>"
        DEPENDS laiue_walk_android laiue_walk_android_manifest
        VERBATIM)
else()
    if(LAIUE_ANDROID_AAPT2 AND LAIUE_ANDROID_ZIPALIGN AND
       LAIUE_ANDROID_APKSIGNER AND LAIUE_ANDROID_FRAMEWORK_JAR)
        message(STATUS
            "Android APK target is disabled: set LAIUE_ANDROID_KEYSTORE "
            "to enable signing")
    else()
        message(STATUS
            "Android APK target is disabled: set LAIUE_ANDROID_AAPT2, "
            "LAIUE_ANDROID_ZIPALIGN, LAIUE_ANDROID_APKSIGNER, "
            "LAIUE_ANDROID_FRAMEWORK_JAR and LAIUE_ANDROID_KEYSTORE")
    endif()
endif()
