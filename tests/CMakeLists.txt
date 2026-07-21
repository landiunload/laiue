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
# Пропуск вместо падения там, где проигрывать нечем: Windows N/Server без
# Media Feature Pack и агенты CI без звуковой карты. Проверки контракта API
# тест выполняет до пропуска, они не зависят от оборудования.
set_tests_properties(laiue.audio.api PROPERTIES SKIP_RETURN_CODE 125)
