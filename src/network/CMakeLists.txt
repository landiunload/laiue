set(laiue_network_sources
    endpoint.c
    network_content.c
    network.h
    protocol.c
    protocol.h
    ../api.h)

set(laiue_network_links)
set(laiue_network_windows_links)

if(TARGET laiue::msquic)
    list(APPEND laiue_network_sources
        certificate_validation.c
        certificate_validation.h
        network_msquic.c)
    list(APPEND laiue_network_links laiue::msquic)
    if(WIN32)
        list(APPEND laiue_network_windows_links
            ws2_32 bcrypt crypt32 ntdll)
    else()
        # Linux certificate pinning validates DER/SAN through OpenSSL X509.
        list(APPEND laiue_network_links OpenSSL::Crypto)
    endif()
else()
    list(APPEND laiue_network_sources
        network_secure.c
        network_unavailable.c)
endif()

laiue_add_module(network
    RUNTIME_ROLE HEADLESS
    SOURCES ${laiue_network_sources}
    LINK ${laiue_network_links}
    WINDOWS_LINK ${laiue_network_windows_links})

if(TARGET laiue::msquic)
    target_compile_definitions(laiue_network PRIVATE LAIUE_HAS_MSQUIC=1)
    if(WIN32 AND LAIUE_MSQUIC_RUNTIME)
        add_custom_command(TARGET laiue_network POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${LAIUE_MSQUIC_RUNTIME}"
                "$<TARGET_FILE_DIR:laiue_network>/msquic.dll"
            COMMENT "Копирование MsQuic runtime")
    elseif(UNIX AND LAIUE_MSQUIC_LIBRARY)
        add_custom_command(TARGET laiue_network POST_BUILD
            COMMAND "${CMAKE_COMMAND}"
                "-DSOURCE=${LAIUE_MSQUIC_LIBRARY}"
                "-DDESTINATION=$<TARGET_FILE_DIR:laiue_network>"
                -P "${PROJECT_SOURCE_DIR}/cmake/CopySharedLibrary.cmake"
            COMMENT "Копирование выбранного MsQuic runtime и SONAME")
    endif()
endif()

# Кодек протокола валидирует недоверенный сетевой ввод и обязан отвергать
# NaN/Infinity, поэтому он компилируется без finite-math-only.
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/protocol.c"
    PROPERTIES COMPILE_OPTIONS "${LAIUE_PRECISE_FP_OPTIONS}")
