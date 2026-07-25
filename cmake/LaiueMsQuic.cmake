include_guard(GLOBAL)

option(LAIUE_ENABLE_MSQUIC "Собирать secure remote transport через MsQuic" ON)
option(LAIUE_REQUIRE_MSQUIC
    "Завершать configure ошибкой, если MsQuic не найден" OFF)
set(LAIUE_MSQUIC_ROOT "" CACHE PATH
    "Префикс установленного MsQuic (include/, lib/ и bin/)")

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
    foreach(cache_entry IN ITEMS
            LAIUE_MSQUIC_INCLUDE_DIR
            LAIUE_MSQUIC_LIBRARY
            LAIUE_MSQUIC_RUNTIME
            LAIUE_MSQUIC_LICENSE
            LAIUE_MSQUIC_NOTICE)
        unset(${cache_entry} CACHE)
    endforeach()
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
    LAIUE_MSQUIC_NOTICE)

set(_laiue_msquic_complete FALSE)
if(LAIUE_MSQUIC_INCLUDE_DIR AND LAIUE_MSQUIC_LIBRARY)
    if(NOT WIN32 OR LAIUE_MSQUIC_RUNTIME)
        set(_laiue_msquic_complete TRUE)
    endif()
endif()

if(_laiue_msquic_complete)
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
        "(headers: ${LAIUE_MSQUIC_INCLUDE_DIR})")
elseif(LAIUE_REQUIRE_MSQUIC)
    message(FATAL_ERROR
        "MsQuic не найден. Установите libmsquic 2.5.9 или задайте "
        "LAIUE_MSQUIC_ROOT")
else()
    message(WARNING
        "MsQuic не найден: secure remote API останется fail-closed. "
        "Для release/CI используйте LAIUE_REQUIRE_MSQUIC=ON.")
endif()
