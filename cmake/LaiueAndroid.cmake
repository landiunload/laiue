include_guard(GLOBAL)

# laiue_add_android_apk(<name>
#     MAIN_TARGET <разделяемая библиотека программы с обычным main>
#     PACKAGE <идентификатор, например dev.laiue.viewer>
#     LABEL <название на экране>
#     [MODULES <цели модулей движка>...]
#     [ASSETS <каталог ресурсов>]
#     [ORIENTATION <screenOrientation, по умолчанию unspecified>]
#     [VERSION_NAME <x.y.z>] [VERSION_CODE <число>])
#
# Цель <name> собирает ${CMAKE_BINARY_DIR}/android/<name>.apk. Разделяемые
# модули из MODULES кладутся в APK отдельными .so, и программа грузит их по
# имени, как на настольных системах: модуль, которого нет в APK, отключает
# только свою технологию. Если окно — разделяемый модуль, библиотека
# активности — оно: его android_main запускает main программы. Статическое
# окно собрано в саму программу, и тогда активность — она. Ключ подписи
# задаёт LAIUE_ANDROID_KEYSTORE: движок ключей не создаёт и не хранит. Без
# него, без build-tools или без android.jar цель не создаётся, а configure
# объясняет почему.

set(LAIUE_ANDROID_AAPT2 "" CACHE FILEPATH "aapt2 used to package Android APKs")
set(LAIUE_ANDROID_ZIPALIGN "" CACHE FILEPATH "zipalign used to package Android APKs")
set(LAIUE_ANDROID_APKSIGNER "" CACHE FILEPATH "apksigner used to sign Android APKs")
set(LAIUE_ANDROID_FRAMEWORK_JAR "" CACHE FILEPATH "android.jar for aapt2")
set(LAIUE_ANDROID_KEYSTORE "" CACHE FILEPATH "Keystore that signs Android APKs")
set(LAIUE_ANDROID_KEYSTORE_PASSWORD "android" CACHE STRING "Password of LAIUE_ANDROID_KEYSTORE")

# Build-tools — инструменты платформы, а не зависимость сборки: если SDK
# известен, берётся новейшая установленная версия, а каждый путь можно
# задать явно для воспроизводимой сборки.
function(_laiue_android_find_tools)
    set(sdk "$ENV{ANDROID_SDK_ROOT}")
    if(sdk STREQUAL "")
        set(sdk "$ENV{ANDROID_HOME}")
    endif()
    set(ndk "${CMAKE_ANDROID_NDK}")
    if(ndk STREQUAL "")
        set(ndk "${ANDROID_NDK}")
    endif()
    if(sdk STREQUAL "" AND NOT ndk STREQUAL "")
        get_filename_component(sdk "${ndk}/../.." ABSOLUTE)
    endif()
    if(sdk STREQUAL "")
        return()
    endif()
    file(GLOB build_tools LIST_DIRECTORIES true "${sdk}/build-tools/*")
    list(SORT build_tools COMPARE NATURAL ORDER DESCENDING)
    list(LENGTH build_tools count)
    if(count GREATER 0)
        list(GET build_tools 0 tools)
        foreach(tool IN ITEMS aapt2 zipalign apksigner)
            string(TOUPPER "${tool}" upper)
            if(LAIUE_ANDROID_${upper} STREQUAL "")
                # apksigner в Windows SDK — пакетный файл.
                foreach(suffix IN ITEMS ".exe" ".bat" "")
                    if(EXISTS "${tools}/${tool}${suffix}" AND
                       NOT IS_DIRECTORY "${tools}/${tool}${suffix}")
                        set(LAIUE_ANDROID_${upper} "${tools}/${tool}${suffix}" CACHE FILEPATH
                            "${tool} used to package Android APKs" FORCE)
                        break()
                    endif()
                endforeach()
            endif()
        endforeach()
    endif()
    if(LAIUE_ANDROID_FRAMEWORK_JAR STREQUAL "")
        file(GLOB jars LIST_DIRECTORIES false "${sdk}/platforms/android-*/android.jar")
        list(SORT jars COMPARE NATURAL ORDER DESCENDING)
        list(LENGTH jars count)
        if(count GREATER 0)
            list(GET jars 0 jar)
            set(LAIUE_ANDROID_FRAMEWORK_JAR "${jar}" CACHE FILEPATH "android.jar for aapt2" FORCE)
        endif()
    endif()
endfunction()

function(laiue_add_android_apk name)
    cmake_parse_arguments(PARSE_ARGV 1 apk ""
        "MAIN_TARGET;PACKAGE;LABEL;ASSETS;ORIENTATION;VERSION_NAME;VERSION_CODE" "MODULES")
    if(NOT ANDROID)
        message(FATAL_ERROR "laiue_add_android_apk needs an Android toolchain")
    endif()
    if(NOT apk_MAIN_TARGET OR NOT apk_PACKAGE OR NOT apk_LABEL)
        message(FATAL_ERROR "laiue_add_android_apk(${name}) needs MAIN_TARGET, PACKAGE and LABEL")
    endif()
    if(NOT TARGET "${apk_MAIN_TARGET}")
        message(FATAL_ERROR "${name}: ${apk_MAIN_TARGET} is not a target")
    endif()
    get_target_property(main_type "${apk_MAIN_TARGET}" TYPE)
    if(NOT main_type MATCHES "^(SHARED|MODULE)_LIBRARY$")
        message(FATAL_ERROR "${name}: ${apk_MAIN_TARGET} must be a shared library: "
            "Android starts programs from libraries, not executables")
    endif()
    if(NOT apk_ORIENTATION)
        set(apk_ORIENTATION unspecified)
    endif()
    if(NOT apk_VERSION_NAME)
        set(apk_VERSION_NAME "${PROJECT_VERSION}")
    endif()
    if(NOT apk_VERSION_NAME)
        set(apk_VERSION_NAME "0.0.0")
    endif()
    if(NOT apk_VERSION_CODE)
        string(REGEX MATCHALL "[0-9]+" parts "${apk_VERSION_NAME}")
        list(APPEND parts 0 0 0)
        list(GET parts 0 major)
        list(GET parts 1 minor)
        list(GET parts 2 patch)
        math(EXPR apk_VERSION_CODE "${major} * 1000000 + ${minor} * 1000 + ${patch} + 1")
    endif()

    set(main_name "$<TARGET_FILE_BASE_NAME:${apk_MAIN_TARGET}>")
    set(libraries "$<TARGET_FILE:${apk_MAIN_TARGET}>")
    set(LAIUE_APK_ACTIVITY_LIBRARY "${main_name}")
    set(main_argument)
    foreach(module IN LISTS apk_MODULES)
        if(NOT TARGET "${module}")
            message(FATAL_ERROR "${name}: ${module} is not a target")
        endif()
        get_target_property(module_type "${module}" TYPE)
        if(NOT module_type MATCHES "^(SHARED|MODULE)_LIBRARY$")
            # Статический модуль уже собран в программу.
            continue()
        endif()
        list(APPEND libraries "$<TARGET_FILE:${module}>")
        if(module STREQUAL "laiue_window" OR module STREQUAL "laiue::window")
            set(LAIUE_APK_ACTIVITY_LIBRARY "$<TARGET_FILE_BASE_NAME:${module}>")
            set(main_argument -MainLibrary "${main_name}")
        endif()
    endforeach()
    list(JOIN libraries "|" library_list)

    # Функцию вызывают и из чужих проектов: файлы берутся рядом с ней.
    set(files "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/android")
    _laiue_android_find_tools()
    set(target_sdk "")
    if(LAIUE_ANDROID_FRAMEWORK_JAR MATCHES "android-([0-9]+)[^/\\]*[/\\]android[.]jar$")
        set(target_sdk "${CMAKE_MATCH_1}")
    endif()
    if(NOT LAIUE_ANDROID_AAPT2 OR NOT LAIUE_ANDROID_ZIPALIGN OR NOT LAIUE_ANDROID_APKSIGNER OR
       NOT target_sdk)
        message(STATUS "${name}: APK disabled, Android build-tools or android.jar not found")
        return()
    endif()
    if(NOT LAIUE_ANDROID_KEYSTORE OR NOT EXISTS "${LAIUE_ANDROID_KEYSTORE}")
        message(STATUS "${name}: APK disabled, set LAIUE_ANDROID_KEYSTORE to sign it")
        return()
    endif()
    # Прежний toolchain-файл NDK, который он выбирает по умолчанию, оставляет
    # CMAKE_SYSTEM_VERSION равным 1, а уровень API кладёт в
    # ANDROID_PLATFORM_LEVEL; встроенная поддержка CMake задаёт оба. С 1 APK
    # ставился бы на системы, где его библиотеки не загрузятся.
    set(min_sdk "${ANDROID_PLATFORM_LEVEL}")
    if(NOT min_sdk)
        set(min_sdk "${CMAKE_SYSTEM_VERSION}")
    endif()
    if(NOT min_sdk MATCHES "^[0-9]+$" OR min_sdk LESS 21)
        message(FATAL_ERROR "${name}: the Android API level is unknown (${min_sdk}); "
            "set ANDROID_PLATFORM, for example android-28")
    endif()

    set(LAIUE_APK_PACKAGE "${apk_PACKAGE}")
    set(LAIUE_APK_LABEL "${apk_LABEL}")
    set(LAIUE_APK_ORIENTATION "${apk_ORIENTATION}")
    set(manifest "${CMAKE_CURRENT_BINARY_DIR}/${name}/AndroidManifest.xml")
    configure_file("${files}/AndroidManifest.xml.in" "${manifest}.in" @ONLY)
    # Имя библиотеки окна известно только при генерации.
    file(GENERATE OUTPUT "${manifest}" INPUT "${manifest}.in")

    set(assets_argument)
    if(apk_ASSETS)
        set(assets_argument -AssetsDirectory "${apk_ASSETS}")
    endif()
    # NDK добавляет отладочные сведения во все конфигурации. В APK идут
    # очищенные копии, как у Android Gradle Plugin, а отладчик берёт полные
    # из каталога сборки.
    set(strip_argument)
    if(CMAKE_STRIP)
        set(strip_argument -Strip "${CMAKE_STRIP}")
    endif()
    find_program(LAIUE_POWERSHELL NAMES pwsh powershell REQUIRED)
    set(output "${CMAKE_BINARY_DIR}/android/${name}.apk")
    add_custom_target(${name}
        COMMAND "${LAIUE_POWERSHELL}" -NoProfile -ExecutionPolicy Bypass -File
            "${files}/package_apk.ps1"
            -Aapt2 "${LAIUE_ANDROID_AAPT2}"
            -Zipalign "${LAIUE_ANDROID_ZIPALIGN}"
            -Apksigner "${LAIUE_ANDROID_APKSIGNER}"
            -FrameworkJar "${LAIUE_ANDROID_FRAMEWORK_JAR}"
            -Manifest "${manifest}"
            -Libraries "${library_list}"
            -Abi "${ANDROID_ABI}"
            ${main_argument}
            ${assets_argument}
            ${strip_argument}
            -Keystore "${LAIUE_ANDROID_KEYSTORE}"
            -KeystorePassword "${LAIUE_ANDROID_KEYSTORE_PASSWORD}"
            -Output "${output}"
            -MinSdk "${min_sdk}"
            -TargetSdk "${target_sdk}"
            -VersionName "${apk_VERSION_NAME}"
            -VersionCode "${apk_VERSION_CODE}"
        DEPENDS "${apk_MAIN_TARGET}" ${apk_MODULES}
        COMMENT "Packaging ${output}"
        VERBATIM)
    set_property(TARGET ${name} PROPERTY LAIUE_ANDROID_APK "${output}")
endfunction()
