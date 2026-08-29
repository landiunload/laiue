cmake_minimum_required(VERSION 3.28)

foreach(required IN ITEMS
        ENGINE_BINARY_DIR CONSUMER_SOURCE_DIR TEST_ROOT CONFIGURATION
        ENGINE_GENERATOR)
    if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR "${required} is required")
    endif()
endforeach()

file(REMOVE_RECURSE "${TEST_ROOT}")
set(prefix "${TEST_ROOT}/prefix")
set(consumer_binary "${TEST_ROOT}/build")

execute_process(
    COMMAND "${CMAKE_COMMAND}" --install "${ENGINE_BINARY_DIR}"
        --config "${CONFIGURATION}"
        --prefix "${prefix}"
        --component Engine
    RESULT_VARIABLE install_result
    OUTPUT_VARIABLE install_output
    ERROR_VARIABLE install_error)
if(NOT install_result EQUAL 0)
    message(FATAL_ERROR
        "Engine install failed:\n${install_output}${install_error}")
endif()

set(configure_command
    "${CMAKE_COMMAND}"
    -S "${CONSUMER_SOURCE_DIR}"
    -B "${consumer_binary}"
    -G "${ENGINE_GENERATOR}"
    "-DCMAKE_PREFIX_PATH=${prefix}")
if(ENGINE_SANITIZERS AND NOT HOST_WIN32)
    list(APPEND configure_command
        "-DCMAKE_C_FLAGS=-fno-omit-frame-pointer -fsanitize=address,undefined"
        "-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address,undefined")
endif()
if(DEFINED ENGINE_GENERATOR_PLATFORM
   AND NOT "${ENGINE_GENERATOR_PLATFORM}" STREQUAL "")
    list(APPEND configure_command -A "${ENGINE_GENERATOR_PLATFORM}")
endif()
if(DEFINED ENGINE_GENERATOR_TOOLSET
   AND NOT "${ENGINE_GENERATOR_TOOLSET}" STREQUAL "")
    list(APPEND configure_command -T "${ENGINE_GENERATOR_TOOLSET}")
endif()
execute_process(
    COMMAND ${configure_command}
    RESULT_VARIABLE configure_result
    OUTPUT_VARIABLE configure_output
    ERROR_VARIABLE configure_error)
if(NOT configure_result EQUAL 0)
    message(FATAL_ERROR
        "Consumer configure failed:\n${configure_output}${configure_error}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${consumer_binary}"
        --config "${CONFIGURATION}"
    RESULT_VARIABLE build_result
    OUTPUT_VARIABLE build_output
    ERROR_VARIABLE build_error)
if(NOT build_result EQUAL 0)
    message(FATAL_ERROR
        "Consumer build failed:\n${build_output}${build_error}")
endif()

if(ENGINE_MULTI_CONFIG)
    set(executable_directory
        "${consumer_binary}/${CONFIGURATION}")
else()
    set(executable_directory "${consumer_binary}")
endif()
if(HOST_WIN32)
    set(executable "${executable_directory}/laiue_consumer.exe")
    file(COPY "${prefix}/bin/" DESTINATION "${executable_directory}")
else()
    set(executable "${executable_directory}/laiue_consumer")
endif()
execute_process(
    COMMAND "${executable}"
    RESULT_VARIABLE run_result
    OUTPUT_VARIABLE run_output
    ERROR_VARIABLE run_error)
if(NOT run_result EQUAL 0)
    message(FATAL_ERROR
        "Installed consumer failed (${run_result}):\n"
        "${run_output}${run_error}")
endif()
