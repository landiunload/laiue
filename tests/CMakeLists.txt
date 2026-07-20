add_executable(laiue_audio_api_test EXCLUDE_FROM_ALL audio_api_test.c)
target_link_libraries(laiue_audio_api_test PRIVATE
    laiue_common
    laiue_runtime
    laiue_audio
    kernel32
)
target_link_options(laiue_audio_api_test PRIVATE
    /ENTRY:AudioApiTestEntryPoint
    /SUBSYSTEM:CONSOLE
)
if(TARGET laiue_runtime_not_running_check)
    add_dependencies(laiue_audio_api_test laiue_runtime_not_running_check)
endif()

add_test(NAME laiue.audio.api COMMAND laiue_audio_api_test)
# Media Foundation может отсутствовать в Windows N/Server без Media Pack.
set_tests_properties(laiue.audio.api PROPERTIES SKIP_RETURN_CODE 125)
