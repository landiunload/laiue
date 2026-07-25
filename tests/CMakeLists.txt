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
