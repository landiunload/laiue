include_guard(GLOBAL)

option(LAIUE_ENABLE_MSQUIC "Собирать secure remote transport через MsQuic" ON)
option(LAIUE_REQUIRE_MSQUIC
    "Завершать configure ошибкой, если MsQuic не найден" OFF)
set(LAIUE_MSQUIC_REQUIRED_VERSION "2.5.9")
set(LAIUE_MSQUIC_REQUIRED_COMMIT
    "87b53085d76bd7920d490a6f226c9999b6614d14")
set(LAIUE_MSQUIC_REQUIRED_QUICTLS_COMMIT
    "ff36838bb69801cad56823159a036977bcbe5c75")
set(LAIUE_MSQUIC_IS_LEAN_PREFIX FALSE)
set(_laiue_msquic_root_default "")
if(DEFINED ENV{LAIUE_MSQUIC_ROOT}
   AND NOT "$ENV{LAIUE_MSQUIC_ROOT}" STREQUAL "")
    set(_laiue_msquic_root_default "$ENV{LAIUE_MSQUIC_ROOT}")
endif()
set(LAIUE_MSQUIC_ROOT "${_laiue_msquic_root_default}" CACHE PATH
    "Префикс установленного MsQuic (include/, lib/ и bin/)")
if(NOT LAIUE_MSQUIC_ROOT
   AND DEFINED ENV{LAIUE_MSQUIC_ROOT}
   AND NOT "$ENV{LAIUE_MSQUIC_ROOT}" STREQUAL "")
    set(LAIUE_MSQUIC_ROOT "$ENV{LAIUE_MSQUIC_ROOT}" CACHE PATH
        "Префикс установленного MsQuic (include/, lib/ и bin/)" FORCE)
endif()
set(LAIUE_MSQUIC_VERSION "" CACHE STRING
    "Проверенная версия MsQuic, если package metadata недоступна")

if(NOT LAIUE_ENABLE_MSQUIC)
    if(LAIUE_REQUIRE_MSQUIC)
        message(FATAL_ERROR
            "LAIUE_REQUIRE_MSQUIC несовместим с LAIUE_ENABLE_MSQUIC=OFF")
    endif()
    return()
endif()

set(_laiue_msquic_include_hints)
set(_laiue_msquic_library_hints)
set(_laiue_msquic_runtime_hints)
# Detection results belong to the current prefix as a unit. Re-run every
# lookup when LAIUE_MSQUIC_ROOT changes so a cached path cannot mix two
# installations or survive an explicit switch back to system packages.
foreach(cache_entry IN ITEMS
        LAIUE_MSQUIC_INCLUDE_DIR
        LAIUE_MSQUIC_LIBRARY
        LAIUE_MSQUIC_RUNTIME
        LAIUE_MSQUIC_LICENSE
        LAIUE_MSQUIC_NOTICE
        LAIUE_MSQUIC_TLS_LICENSE
        LAIUE_MSQUIC_VERSION_FILE
        LAIUE_MSQUIC_BUILD_METADATA)
    unset(${cache_entry} CACHE)
endforeach()
if(LAIUE_MSQUIC_ROOT)
    list(APPEND _laiue_msquic_include_hints
        "${LAIUE_MSQUIC_ROOT}/include"
        "${LAIUE_MSQUIC_ROOT}/build/native/include")
    list(APPEND _laiue_msquic_library_hints
        "${LAIUE_MSQUIC_ROOT}/lib"
        "${LAIUE_MSQUIC_ROOT}/lib/x64"
        "${LAIUE_MSQUIC_ROOT}/build/native/lib/x64"
        "${LAIUE_MSQUIC_ROOT}/runtimes/win-x64/native")
    list(APPEND _laiue_msquic_runtime_hints
        "${LAIUE_MSQUIC_ROOT}/bin"
        "${LAIUE_MSQUIC_ROOT}/bin/x64"
        "${LAIUE_MSQUIC_ROOT}/build/native/bin/x64"
        "${LAIUE_MSQUIC_ROOT}/runtimes/win-x64/native")

    # Явный prefix является единицей доверия: нельзя незаметно смешать
    # header/import library из него с DLL или notices из PATH другой версии.
    find_path(LAIUE_MSQUIC_INCLUDE_DIR
        NAMES msquic.h
        PATHS ${_laiue_msquic_include_hints}
        NO_DEFAULT_PATH)
    find_library(LAIUE_MSQUIC_LIBRARY
        NAMES msquic
        PATHS ${_laiue_msquic_library_hints}
        NO_DEFAULT_PATH)
    find_file(LAIUE_MSQUIC_LICENSE
        NAMES LICENSE LICENSE.txt copyright
        PATHS
            "${LAIUE_MSQUIC_ROOT}"
            "${LAIUE_MSQUIC_ROOT}/share/doc/libmsquic"
        NO_DEFAULT_PATH)
    find_file(LAIUE_MSQUIC_NOTICE
        NAMES THIRD-PARTY-NOTICES THIRD-PARTY-NOTICES.txt
        PATHS
            "${LAIUE_MSQUIC_ROOT}"
            "${LAIUE_MSQUIC_ROOT}/share/doc/libmsquic"
        NO_DEFAULT_PATH)
    find_file(LAIUE_MSQUIC_TLS_LICENSE
        NAMES QUIC-TLS-LICENSE
        PATHS "${LAIUE_MSQUIC_ROOT}"
        NO_DEFAULT_PATH)
    find_file(LAIUE_MSQUIC_VERSION_FILE
        NAMES VERSION
        PATHS "${LAIUE_MSQUIC_ROOT}"
        NO_DEFAULT_PATH)
    find_file(LAIUE_MSQUIC_BUILD_METADATA
        NAMES BUILD-METADATA
        PATHS "${LAIUE_MSQUIC_ROOT}"
        NO_DEFAULT_PATH)
    if(WIN32)
        find_file(LAIUE_MSQUIC_RUNTIME
            NAMES msquic.dll
            PATHS ${_laiue_msquic_runtime_hints}
            NO_DEFAULT_PATH)
    endif()
else()
    find_path(LAIUE_MSQUIC_INCLUDE_DIR NAMES msquic.h)
    find_library(LAIUE_MSQUIC_LIBRARY NAMES msquic)
    find_file(LAIUE_MSQUIC_LICENSE
        NAMES LICENSE copyright
        PATHS
            "/usr/share/doc/libmsquic"
        NO_DEFAULT_PATH)
    find_file(LAIUE_MSQUIC_NOTICE
        NAMES THIRD-PARTY-NOTICES THIRD-PARTY-NOTICES.txt
        PATHS
            "/usr/share/doc/libmsquic"
        NO_DEFAULT_PATH)
    find_file(LAIUE_MSQUIC_TLS_LICENSE
        NAMES QUIC-TLS-LICENSE
        PATHS "/usr/share/doc/libmsquic"
        NO_DEFAULT_PATH)
    find_file(LAIUE_MSQUIC_VERSION_FILE
        NAMES VERSION
        PATHS "/usr/share/doc/libmsquic"
        NO_DEFAULT_PATH)
    find_file(LAIUE_MSQUIC_BUILD_METADATA
        NAMES BUILD-METADATA
        PATHS "/usr/share/doc/libmsquic"
        NO_DEFAULT_PATH)
    if(WIN32)
        # Для system install разрешён обычный поиск, но все release jobs
        # используют строгий проверенный LAIUE_MSQUIC_ROOT.
        find_file(LAIUE_MSQUIC_RUNTIME NAMES msquic.dll)
    endif()
endif()

mark_as_advanced(
    LAIUE_MSQUIC_INCLUDE_DIR
    LAIUE_MSQUIC_LIBRARY
    LAIUE_MSQUIC_RUNTIME
    LAIUE_MSQUIC_LICENSE
    LAIUE_MSQUIC_NOTICE
    LAIUE_MSQUIC_TLS_LICENSE
    LAIUE_MSQUIC_VERSION_FILE
    LAIUE_MSQUIC_BUILD_METADATA)

function(_laiue_msquic_metadata_value metadata_file metadata_key
         output_variable)
    file(STRINGS "${metadata_file}" _laiue_metadata_matches
        REGEX "^${metadata_key}=")
    list(LENGTH _laiue_metadata_matches _laiue_metadata_count)
    if(NOT _laiue_metadata_count EQUAL 1)
        message(FATAL_ERROR
            "MsQuic BUILD-METADATA требует ровно одно поле "
            "${metadata_key}=...")
    endif()
    list(GET _laiue_metadata_matches 0 _laiue_metadata_line)
    string(REGEX REPLACE "^[^=]*=" "" _laiue_metadata_value
        "${_laiue_metadata_line}")
    if(_laiue_metadata_value STREQUAL "")
        message(FATAL_ERROR
            "Пустое поле ${metadata_key} в MsQuic BUILD-METADATA")
    endif()
    set(${output_variable} "${_laiue_metadata_value}" PARENT_SCOPE)
endfunction()

function(_laiue_msquic_require_metadata metadata_file metadata_key
         expected_value)
    _laiue_msquic_metadata_value(
        "${metadata_file}" "${metadata_key}" _laiue_metadata_actual)
    if(NOT _laiue_metadata_actual STREQUAL "${expected_value}")
        message(FATAL_ERROR
            "MsQuic BUILD-METADATA: ${metadata_key}="
            "${_laiue_metadata_actual}, требуется ${expected_value}")
    endif()
endfunction()

function(_laiue_msquic_require_metadata_sha256 metadata_file
         metadata_key provenance_file)
    _laiue_msquic_metadata_value(
        "${metadata_file}" "${metadata_key}" _laiue_expected_sha256)
    string(LENGTH "${_laiue_expected_sha256}" _laiue_sha256_length)
    if(NOT _laiue_sha256_length EQUAL 64 OR
       NOT _laiue_expected_sha256 MATCHES "^[0-9a-f]+$")
        message(FATAL_ERROR
            "Некорректное поле ${metadata_key} в MsQuic "
            "BUILD-METADATA")
    endif()
    file(SHA256 "${provenance_file}" _laiue_actual_sha256)
    if(NOT "${_laiue_actual_sha256}" STREQUAL
           "${_laiue_expected_sha256}")
        message(FATAL_ERROR
            "${provenance_file} не совпадает с MsQuic "
            "BUILD-METADATA")
    endif()
endfunction()

set(_laiue_msquic_complete FALSE)
if(LAIUE_MSQUIC_INCLUDE_DIR AND LAIUE_MSQUIC_LIBRARY)
    if(NOT WIN32 OR LAIUE_MSQUIC_RUNTIME)
        set(_laiue_msquic_complete TRUE)
    endif()
endif()

if(_laiue_msquic_complete)
    if(UNIX AND LAIUE_REQUIRE_MSQUIC
       AND NOT IS_SYMLINK "${LAIUE_MSQUIC_LIBRARY}")
        message(FATAL_ERROR
            "Release prefix должен предоставлять libmsquic.so как начало "
            "проверенной SONAME symlink chain; выбран обычный файл: "
            "${LAIUE_MSQUIC_LIBRARY}")
    endif()
    set(_laiue_msquic_detected_version "")
    if(LAIUE_MSQUIC_ROOT)
        if(LAIUE_MSQUIC_VERSION_FILE)
            file(READ "${LAIUE_MSQUIC_VERSION_FILE}"
                _laiue_msquic_version_text LIMIT 256)
            string(REGEX MATCH
                "([0-9]+\\.[0-9]+\\.[0-9]+)"
                _laiue_msquic_version_match
                "${_laiue_msquic_version_text}")
            set(_laiue_msquic_detected_version "${CMAKE_MATCH_1}")
        endif()
        if(NOT _laiue_msquic_detected_version)
            file(GLOB _laiue_msquic_nuspecs
                LIST_DIRECTORIES FALSE
                "${LAIUE_MSQUIC_ROOT}/*.nuspec")
            list(SORT _laiue_msquic_nuspecs)
            foreach(_laiue_msquic_nuspec IN LISTS
                    _laiue_msquic_nuspecs)
                file(READ "${_laiue_msquic_nuspec}"
                    _laiue_msquic_nuspec_text LIMIT 16384)
                string(REGEX MATCH
                    "<version>[ \t\r\n]*([0-9]+\\.[0-9]+\\.[0-9]+)"
                    _laiue_msquic_nuspec_match
                    "${_laiue_msquic_nuspec_text}")
                if(_laiue_msquic_nuspec_match)
                    set(_laiue_msquic_detected_version
                        "${CMAKE_MATCH_1}")
                    break()
                endif()
            endforeach()
        endif()
    endif()
    if(NOT _laiue_msquic_detected_version AND UNIX
       AND NOT LAIUE_MSQUIC_ROOT)
        find_package(PkgConfig QUIET)
        if(PkgConfig_FOUND)
            pkg_check_modules(_LAIUE_MSQUIC_PC QUIET libmsquic)
            if(NOT _LAIUE_MSQUIC_PC_FOUND)
                pkg_check_modules(_LAIUE_MSQUIC_PC QUIET msquic)
            endif()
            if(_LAIUE_MSQUIC_PC_FOUND)
                set(_laiue_msquic_detected_version
                    "${_LAIUE_MSQUIC_PC_VERSION}")
            endif()
        endif()
    endif()
    if(NOT _laiue_msquic_detected_version AND
       LAIUE_MSQUIC_LIBRARY MATCHES
           "\\.so\\.([0-9]+\\.[0-9]+\\.[0-9]+)")
        set(_laiue_msquic_detected_version "${CMAKE_MATCH_1}")
    endif()
    if(LAIUE_MSQUIC_VERSION)
        if(_laiue_msquic_detected_version AND
           NOT _laiue_msquic_detected_version VERSION_EQUAL
               LAIUE_MSQUIC_VERSION)
            message(FATAL_ERROR
                "Указанная версия MsQuic ${LAIUE_MSQUIC_VERSION} "
                "противоречит metadata выбранного runtime "
                "${_laiue_msquic_detected_version}")
        elseif(NOT _laiue_msquic_detected_version)
            set(_laiue_msquic_detected_version
                "${LAIUE_MSQUIC_VERSION}")
        endif()
    endif()
    if(NOT _laiue_msquic_detected_version)
        if(LAIUE_REQUIRE_MSQUIC)
            message(FATAL_ERROR
                "Версию MsQuic нельзя проверить. Используйте prefix с "
                "VERSION/package metadata либо задайте "
                "-DLAIUE_MSQUIC_VERSION=${LAIUE_MSQUIC_REQUIRED_VERSION} "
                "после проверки пакета/hash.")
        endif()
        message(WARNING
            "Версия найденного MsQuic не подтверждена; release-сборки "
            "обязаны использовать ${LAIUE_MSQUIC_REQUIRED_VERSION}.")
    elseif(NOT _laiue_msquic_detected_version VERSION_EQUAL
               LAIUE_MSQUIC_REQUIRED_VERSION)
        message(FATAL_ERROR
            "Требуется MsQuic ${LAIUE_MSQUIC_REQUIRED_VERSION}, найден "
            "${_laiue_msquic_detected_version}")
    endif()
    if(LAIUE_REQUIRE_MSQUIC AND
       (NOT LAIUE_MSQUIC_LICENSE OR NOT LAIUE_MSQUIC_NOTICE))
        message(FATAL_ERROR
            "Release bundle требует LICENSE и THIRD-PARTY-NOTICES "
            "из того же проверенного MsQuic prefix")
    endif()
    if(UNIX AND LAIUE_MSQUIC_BUILD_METADATA)
        file(SIZE "${LAIUE_MSQUIC_BUILD_METADATA}"
            _laiue_msquic_metadata_size)
        if(_laiue_msquic_metadata_size GREATER 32768)
            message(FATAL_ERROR
                "MsQuic BUILD-METADATA превышает 32 KiB")
        endif()
        file(STRINGS "${LAIUE_MSQUIC_BUILD_METADATA}"
            _laiue_msquic_lean_markers
            REGEX "^(format=laiue-msquic-build-metadata-v1|profile=laiue-lean)$")
        if(_laiue_msquic_lean_markers)
            if(NOT LAIUE_MSQUIC_VERSION_FILE OR
               NOT LAIUE_MSQUIC_TLS_LICENSE OR
               NOT LAIUE_MSQUIC_LICENSE OR
               NOT LAIUE_MSQUIC_NOTICE)
                message(FATAL_ERROR
                    "Lean MsQuic prefix требует VERSION, LICENSE, "
                    "THIRD-PARTY-NOTICES и QUIC-TLS-LICENSE")
            endif()
            _laiue_msquic_require_metadata(
                "${LAIUE_MSQUIC_BUILD_METADATA}" format
                laiue-msquic-build-metadata-v1)
            _laiue_msquic_require_metadata(
                "${LAIUE_MSQUIC_BUILD_METADATA}" profile laiue-lean)
            _laiue_msquic_require_metadata(
                "${LAIUE_MSQUIC_BUILD_METADATA}" version
                "${LAIUE_MSQUIC_REQUIRED_VERSION}")
            _laiue_msquic_require_metadata(
                "${LAIUE_MSQUIC_BUILD_METADATA}" source_commit
                "${LAIUE_MSQUIC_REQUIRED_COMMIT}")
            _laiue_msquic_require_metadata(
                "${LAIUE_MSQUIC_BUILD_METADATA}" quictls_commit
                "${LAIUE_MSQUIC_REQUIRED_QUICTLS_COMMIT}")
            _laiue_msquic_require_metadata(
                "${LAIUE_MSQUIC_BUILD_METADATA}" architecture x86_64)
            _laiue_msquic_require_metadata(
                "${LAIUE_MSQUIC_BUILD_METADATA}" libc
                "${LAIUE_LINUX_LIBC}")
            foreach(_laiue_msquic_disabled_feature IN ITEMS
                    xdp logging tools tests perf embedded_git_hash)
                _laiue_msquic_require_metadata(
                    "${LAIUE_MSQUIC_BUILD_METADATA}"
                    "${_laiue_msquic_disabled_feature}" OFF)
            endforeach()
            _laiue_msquic_require_metadata(
                "${LAIUE_MSQUIC_BUILD_METADATA}" tls quictls)
            _laiue_msquic_require_metadata(
                "${LAIUE_MSQUIC_BUILD_METADATA}"
                system_libcrypto ON)
            _laiue_msquic_require_metadata(
                "${LAIUE_MSQUIC_BUILD_METADATA}"
                strip strip-unneeded)
            _laiue_msquic_metadata_value(
                "${LAIUE_MSQUIC_BUILD_METADATA}" runtime_sha256
                _laiue_msquic_expected_sha256)
            string(LENGTH "${_laiue_msquic_expected_sha256}"
                _laiue_msquic_sha256_length)
            if(NOT _laiue_msquic_sha256_length EQUAL 64 OR
               NOT _laiue_msquic_expected_sha256 MATCHES
                   "^[0-9a-f]+$")
                message(FATAL_ERROR
                    "Некорректный runtime_sha256 в MsQuic "
                    "BUILD-METADATA")
            endif()
            file(SHA256 "${LAIUE_MSQUIC_LIBRARY}"
                _laiue_msquic_actual_sha256)
            if(NOT "${_laiue_msquic_actual_sha256}" STREQUAL
                   "${_laiue_msquic_expected_sha256}")
                message(FATAL_ERROR
                    "Выбранный libmsquic не совпадает с "
                    "BUILD-METADATA SHA-256")
            endif()
            _laiue_msquic_require_metadata_sha256(
                "${LAIUE_MSQUIC_BUILD_METADATA}" license_sha256
                "${LAIUE_MSQUIC_LICENSE}")
            _laiue_msquic_require_metadata_sha256(
                "${LAIUE_MSQUIC_BUILD_METADATA}"
                third_party_notices_sha256
                "${LAIUE_MSQUIC_NOTICE}")
            _laiue_msquic_require_metadata_sha256(
                "${LAIUE_MSQUIC_BUILD_METADATA}"
                quictls_license_sha256
                "${LAIUE_MSQUIC_TLS_LICENSE}")
            set(LAIUE_MSQUIC_IS_LEAN_PREFIX TRUE)
            message(STATUS
                "MsQuic prefix: проверенный laiue-lean "
                "(${LAIUE_LINUX_LIBC})")
        else()
            message(STATUS
                "MsQuic BUILD-METADATA имеет сторонний формат и не "
                "включается в laiue bundle")
            set(LAIUE_MSQUIC_BUILD_METADATA "")
        endif()
    endif()
    set(LAIUE_MSQUIC_DETECTED_VERSION
        "${_laiue_msquic_detected_version}" CACHE INTERNAL
        "Фактически проверенная версия MsQuic" FORCE)

    add_library(laiue_msquic SHARED IMPORTED GLOBAL)
    add_library(laiue::msquic ALIAS laiue_msquic)
    set_target_properties(laiue_msquic PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${LAIUE_MSQUIC_INCLUDE_DIR}")
    if(WIN32)
        set_target_properties(laiue_msquic PROPERTIES
            IMPORTED_IMPLIB "${LAIUE_MSQUIC_LIBRARY}")
        set_target_properties(laiue_msquic PROPERTIES
            IMPORTED_LOCATION "${LAIUE_MSQUIC_RUNTIME}")
    else()
        set_target_properties(laiue_msquic PROPERTIES
            IMPORTED_LOCATION "${LAIUE_MSQUIC_LIBRARY}")
    endif()
    message(STATUS
        "MsQuic: ${LAIUE_MSQUIC_LIBRARY} "
        "(headers: ${LAIUE_MSQUIC_INCLUDE_DIR}, "
        "version: ${_laiue_msquic_detected_version})")
elseif(LAIUE_REQUIRE_MSQUIC)
    message(FATAL_ERROR
        "MsQuic не найден. Установите libmsquic 2.5.9 или задайте "
        "LAIUE_MSQUIC_ROOT")
else()
    message(WARNING
        "MsQuic не найден: secure remote API останется fail-closed. "
        "Для release/CI используйте LAIUE_REQUIRE_MSQUIC=ON.")
endif()
