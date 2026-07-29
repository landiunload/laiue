cmake_minimum_required(VERSION 3.28)

if(NOT DEFINED SOURCE_ROOT OR SOURCE_ROOT STREQUAL "")
    message(FATAL_ERROR "SOURCE_ROOT is required")
endif()

cmake_path(ABSOLUTE_PATH SOURCE_ROOT
    NORMALIZE
    OUTPUT_VARIABLE source_root)

find_program(git_executable NAMES git REQUIRED)

execute_process(
    COMMAND "${git_executable}" -C "${source_root}"
        rev-parse --show-toplevel
    RESULT_VARIABLE repository_result
    OUTPUT_VARIABLE repository_root
    ERROR_VARIABLE repository_error
    OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT repository_result EQUAL 0)
    message(FATAL_ERROR
        "Source-tree verification could not open ${source_root} as a "
        "Git repository:\n${repository_error}")
endif()

execute_process(
    COMMAND "${git_executable}" -C "${source_root}" diff --check
    RESULT_VARIABLE diff_check_result
    OUTPUT_VARIABLE diff_check_output
    ERROR_VARIABLE diff_check_error)
if(NOT diff_check_result EQUAL 0)
    message(FATAL_ERROR
        "Source tree contains whitespace errors:\n"
        "${diff_check_output}${diff_check_error}")
endif()

execute_process(
    COMMAND "${git_executable}" -C "${source_root}"
        status --porcelain --untracked-files=all
    RESULT_VARIABLE status_result
    OUTPUT_VARIABLE status_output
    ERROR_VARIABLE status_error)
if(NOT status_result EQUAL 0)
    message(FATAL_ERROR
        "Source-tree verification failed for ${repository_root}:\n"
        "${status_error}")
endif()

if(NOT status_output STREQUAL "")
    execute_process(
        COMMAND "${git_executable}" -C "${source_root}" diff --stat
        OUTPUT_VARIABLE diff_stat
        ERROR_VARIABLE diff_stat_error)
    message(FATAL_ERROR
        "Build changed the checked-out source tree:\n"
        "${status_output}"
        "Tracked diff summary:\n${diff_stat}${diff_stat_error}")
endif()

message(STATUS "Source tree is unchanged: ${repository_root}")
