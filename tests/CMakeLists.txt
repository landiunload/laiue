add_executable(laiue_audio_api_test audio_api_test.c)
target_link_libraries(laiue_audio_api_test PRIVATE
    laiue_common
    laiue_runtime
    laiue_audio
    kernel32
    # waveOutGetNumDevs: проверка наличия устройства вывода звука
    winmm
)
target_link_options(laiue_audio_api_test PRIVATE
    /ENTRY:AudioApiTestEntryPoint
    /SUBSYSTEM:CONSOLE
)
if(TARGET laiue_runtime_not_running_check)
    add_dependencies(laiue_audio_api_test laiue_runtime_not_running_check)
endif()

add_test(NAME laiue.audio.api COMMAND laiue_audio_api_test)

# protocol.c компилируется в тест напрямую: кодек не экспортируется из
# laiue_network, и расширять ABI DLL ради проверок нельзя.
add_executable(laiue_protocol_test
    protocol_test.c
    ../src/network/protocol.c)
# Та же точная модель FP, что и в боевой сборке protocol.c (см.
# src/network/CMakeLists.txt): иначе тест мерил бы не те правила, что
# работают в игре. Тест TestNonFiniteRejected опирается ровно на это.
set_source_files_properties(../src/network/protocol.c
    PROPERTIES COMPILE_OPTIONS "/fp:precise")
target_link_libraries(laiue_protocol_test PRIVATE
    laiue_common
    laiue_runtime
    kernel32
)
target_link_options(laiue_protocol_test PRIVATE
    /ENTRY:ProtocolTestEntryPoint
    /SUBSYSTEM:CONSOLE
)
if(TARGET laiue_runtime_not_running_check)
    add_dependencies(laiue_protocol_test laiue_runtime_not_running_check)
endif()

add_test(NAME laiue.network.protocol COMMAND laiue_protocol_test)

# infinite_coord.c компилируется в тест напрямую по той же причине, что и
# protocol.c: арифметика координат — внутренняя логика laiue_world и наружу
# не экспортируется.
add_executable(laiue_infinite_coord_test
    infinite_coord_test.c
    ../src/world/infinite_coord.c)
target_link_libraries(laiue_infinite_coord_test PRIVATE
    laiue_common
    laiue_runtime
    kernel32
)
target_link_options(laiue_infinite_coord_test PRIVATE
    /ENTRY:CoordTestEntryPoint
    /SUBSYSTEM:CONSOLE
)
if(TARGET laiue_runtime_not_running_check)
    add_dependencies(laiue_infinite_coord_test laiue_runtime_not_running_check)
endif()

add_test(NAME laiue.world.infinite_coord COMMAND laiue_infinite_coord_test)
# Пропуск вместо падения там, где проигрывать нечем: Windows N/Server без
# Media Feature Pack и агенты CI без звуковой карты. Проверки контракта API
# тест выполняет до пропуска, они не зависят от оборудования.
set_tests_properties(laiue.audio.api PROPERTIES SKIP_RETURN_CODE 125)
