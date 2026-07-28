function(laiue_configure_portable_test target_name windows_entry)
    target_link_libraries(${target_name} PRIVATE
        laiue_common
        laiue::platform_support)
    if(WIN32)
        target_link_libraries(${target_name} PRIVATE kernel32)
        target_link_options(${target_name} PRIVATE
            "/ENTRY:${windows_entry}"
            /SUBSYSTEM:CONSOLE)
    endif()
    if(TARGET laiue_runtime_not_running_check)
        add_dependencies(${target_name} laiue_runtime_not_running_check)
    endif()
endfunction()

if(WIN32 AND TARGET laiue_audio)
    add_executable(laiue_audio_api_test audio_api_test.c)
    target_link_libraries(laiue_audio_api_test PRIVATE
        laiue_common
        laiue::platform_support
        laiue_audio
        kernel32
        # waveOutGetNumDevs: проверка наличия устройства вывода звука
        winmm)
    target_link_options(laiue_audio_api_test PRIVATE
        /ENTRY:AudioApiTestEntryPoint
        /SUBSYSTEM:CONSOLE)
    if(TARGET laiue_runtime_not_running_check)
        add_dependencies(laiue_audio_api_test laiue_runtime_not_running_check)
    endif()

    add_test(NAME laiue.audio.api COMMAND laiue_audio_api_test)
    # Пропуск вместо падения там, где проигрывать нечем: Windows N/Server без
    # Media Feature Pack и агенты CI без звуковой карты. Проверки контракта
    # API тест выполняет до пропуска, они не зависят от оборудования.
    set_tests_properties(laiue.audio.api PROPERTIES SKIP_RETURN_CODE 125)
endif()

# protocol.c компилируется в тест напрямую: кодек не экспортируется из
# laiue_network, и расширять ABI DLL ради проверок нельзя.
add_executable(laiue_protocol_test
    protocol_test.c
    ../src/network/endpoint.c
    ../src/network/protocol.c)
# Та же точная модель FP, что и в боевой сборке protocol.c (см.
# src/network/CMakeLists.txt): иначе тест мерил бы не те правила, что
# работают в игре. Тест TestNonFiniteRejected опирается ровно на это.
set_source_files_properties(../src/network/protocol.c
    PROPERTIES COMPILE_OPTIONS "${LAIUE_PRECISE_FP_OPTIONS}")
target_compile_definitions(laiue_protocol_test PRIVATE
    LAIUE_BUILD_NETWORK)
laiue_configure_portable_test(
    laiue_protocol_test ProtocolTestEntryPoint)

add_test(NAME laiue.network.protocol COMMAND laiue_protocol_test)

add_executable(laiue_certificate_validation_test
    certificate_validation_test.c
    ../src/network/certificate_validation.c
    ../src/network/endpoint.c)
target_compile_definitions(laiue_certificate_validation_test PRIVATE
    LAIUE_BUILD_NETWORK)
if(WIN32)
    target_link_libraries(laiue_certificate_validation_test PRIVATE
        crypt32
        ws2_32)
endif()
laiue_configure_portable_test(
    laiue_certificate_validation_test
    CertificateValidationTestEntryPoint)

add_test(NAME laiue.network.certificate
    COMMAND laiue_certificate_validation_test)

if(UNIX AND TARGET laiue_msquic)
    find_program(laiue_test_openssl_executable NAMES openssl)
    if(laiue_test_openssl_executable)
        add_executable(laiue_quic_integration_test
            quic_integration_test.c)
        target_link_libraries(laiue_quic_integration_test PRIVATE
            laiue_network)
        laiue_configure_portable_test(
            laiue_quic_integration_test
            QuicIntegrationTestEntryPoint)

        add_test(NAME laiue.network.quic.integration
            COMMAND "${CMAKE_COMMAND}"
                "-DOPENSSL_EXECUTABLE=${laiue_test_openssl_executable}"
                "-DTEST_EXECUTABLE=$<TARGET_FILE:laiue_quic_integration_test>"
                "-DRUNTIME_DIRECTORY=${CMAKE_CURRENT_BINARY_DIR}/quic-integration-runtime/$<CONFIG>"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/run_quic_integration_test.cmake")
        set_tests_properties(laiue.network.quic.integration PROPERTIES
            RESOURCE_LOCK laiue_quic_listener
            TIMEOUT 60)
    endif()
endif()

add_executable(laiue_server_config_test
    server_config_test.c
    ../src/server/server_config.c)
laiue_configure_portable_test(
    laiue_server_config_test ServerConfigTestEntryPoint)
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/empty-config")

add_test(NAME laiue.server.config.ipv6
    COMMAND laiue_server_config_test)
set_tests_properties(laiue.server.config.ipv6 PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/empty-config"
    ENVIRONMENT
        "LAIUE_TEST_CASE=valid;LAIUE_SERVER_PORT=30000;LAIUE_SERVER_MAXIMUM_PEERS=7;LAIUE_SERVER_WORLD_SEED=-12345;LAIUE_SERVER_ALLOW_CONTENT_DOWNLOADS=true;LAIUE_SERVER_ADDRESS_FAMILY=ipv6;LAIUE_SERVER_LISTEN_ADDRESS=::1;LAIUE_SERVER_CERTIFICATE_FILE=cert.pem;LAIUE_SERVER_PRIVATE_KEY_FILE=key.pem")

add_test(NAME laiue.server.config.invalid_dual
    COMMAND laiue_server_config_test)
set_tests_properties(laiue.server.config.invalid_dual PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/empty-config"
    ENVIRONMENT
        "LAIUE_TEST_CASE=invalid-dual;LAIUE_SERVER_ADDRESS_FAMILY=dual;LAIUE_SERVER_LISTEN_ADDRESS=127.0.0.1")

# infinite_coord.c компилируется в тест напрямую по той же причине, что и
# protocol.c: арифметика координат — внутренняя логика laiue_world и наружу
# не экспортируется.
add_executable(laiue_infinite_coord_test
    infinite_coord_test.c
    ../src/world/infinite_coord.c)
laiue_configure_portable_test(
    laiue_infinite_coord_test CoordTestEntryPoint)

add_test(NAME laiue.world.infinite_coord COMMAND laiue_infinite_coord_test)

add_executable(laiue_world_snapshot_test world_snapshot_test.c)
target_link_libraries(laiue_world_snapshot_test PRIVATE laiue_world)
laiue_configure_portable_test(
    laiue_world_snapshot_test WorldSnapshotTestEntryPoint)

add_test(NAME laiue.world.snapshot COMMAND laiue_world_snapshot_test)

# Харнесс-доказательство детерминизма физики. Сим-модули компилируются прямо
# в тест и закреплены за /fp:precise (без FMA-контракции): глобальный /fp:fast
# ломает побитовую воспроизводимость. LAIUE_BUILD_* переводят экспортные
# макросы в dllexport, чтобы функции определялись, а не импортировались.
set(laiue_determinism_sources
    determinism_test.c
    ../src/gameplay/player_controller.c
    ../src/gameplay/player_jump.c
    ../src/gameplay/player_locomotion.c
    ../src/gameplay/player_stance.c
    ../src/physics/voxel_body.c)
add_executable(laiue_determinism_test ${laiue_determinism_sources})
# Те же опции, что и в боевой сборке этих модулей (см.
# cmake/LaiueToolchain.cmake): тест обязан мерить те правила, что работают
# в игре, иначе он стережёт не то.
set_source_files_properties(${laiue_determinism_sources}
    PROPERTIES COMPILE_OPTIONS "${LAIUE_DETERMINISTIC_FP_OPTIONS}")
target_compile_definitions(laiue_determinism_test PRIVATE
    LAIUE_BUILD_GAMEPLAY
    LAIUE_BUILD_PHYSICS)
laiue_configure_portable_test(
    laiue_determinism_test DeterminismTestEntryPoint)

add_test(NAME laiue.physics.determinism COMMAND laiue_determinism_test)

# content_format.c компилируется в тест напрямую по той же причине, что
# protocol.c и infinite_coord.c: правила именования содержимого — внутренний
# контракт laiue_content, и расширять ABI DLL ради проверок нельзя.
# LAIUE_BUILD_CONTENT переводит экспортные макросы в dllexport, чтобы функции
# определялись в тесте, а не импортировались из DLL.
add_executable(laiue_content_format_test
    content_format_test.c
    ../src/content/content_format.c)
target_compile_definitions(laiue_content_format_test PRIVATE
    LAIUE_BUILD_CONTENT)
laiue_configure_portable_test(
    laiue_content_format_test ContentFormatTestEntryPoint)

add_test(NAME laiue.content.format COMMAND laiue_content_format_test)

add_executable(laiue_mod_manifest_test mod_manifest_test.c)
target_compile_definitions(laiue_mod_manifest_test PRIVATE LAIUE_BUILD_MOD)
target_include_directories(laiue_mod_manifest_test PRIVATE
    "${PROJECT_SOURCE_DIR}/sdk")
target_link_libraries(laiue_mod_manifest_test PRIVATE
    laiue_content)
laiue_configure_portable_test(
    laiue_mod_manifest_test ModManifestTestEntryPoint)

add_test(NAME laiue.mod.manifest COMMAND laiue_mod_manifest_test)

if(LAIUE_BUILD_EXAMPLE_MODS)
    get_property(laiue_test_example_mod_targets GLOBAL PROPERTY
        LAIUE_EXAMPLE_MOD_TARGETS)
    if(laiue_test_example_mod_targets)
        add_executable(laiue_mod_binary_load_test
            mod_binary_load_test.c)
        add_dependencies(laiue_mod_binary_load_test
            ${laiue_test_example_mod_targets})
        laiue_configure_portable_test(
            laiue_mod_binary_load_test
            ModBinaryLoadTestEntryPoint)
        add_test(NAME laiue.mod.binary_load
            COMMAND laiue_mod_binary_load_test)

        add_executable(laiue_mod_package_hash_test
            mod_package_hash_test.c)
        target_compile_definitions(laiue_mod_package_hash_test PRIVATE
            LAIUE_BUILD_MOD)
        target_include_directories(laiue_mod_package_hash_test PRIVATE
            "${PROJECT_SOURCE_DIR}/sdk")
        target_link_libraries(laiue_mod_package_hash_test PRIVATE
            laiue_content)
        add_dependencies(laiue_mod_package_hash_test
            ${laiue_test_example_mod_targets})
        laiue_configure_portable_test(
            laiue_mod_package_hash_test
            ModPackageHashTestEntryPoint)
        add_test(NAME laiue.mod.package_hash
            COMMAND laiue_mod_package_hash_test)
    endif()
endif()
