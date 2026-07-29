cmake_minimum_required(VERSION 3.28)

foreach(required_variable IN ITEMS
        VERIFY_SCRIPT
        TEST_ROOT
        GIT_EXECUTABLE)
    if(NOT DEFINED ${required_variable} OR
       "${${required_variable}}" STREQUAL "")
        message(FATAL_ERROR "${required_variable} is required")
    endif()
endforeach()

file(REMOVE_RECURSE "${TEST_ROOT}")
file(MAKE_DIRECTORY
    "${TEST_ROOT}/repository"
    "${TEST_ROOT}/outside"
    "${TEST_ROOT}/hooks")
set(repository "${TEST_ROOT}/repository")
set(tracked_file "${repository}/tracked.txt")

execute_process(
    COMMAND "${GIT_EXECUTABLE}" init --quiet "${repository}"
    RESULT_VARIABLE init_result
    ERROR_VARIABLE init_error)
if(NOT init_result EQUAL 0)
    message(FATAL_ERROR "Could not initialize test repository:\n${init_error}")
endif()

file(WRITE "${tracked_file}" "checked in\n")
execute_process(
    COMMAND "${GIT_EXECUTABLE}" -C "${repository}" add tracked.txt
    RESULT_VARIABLE add_result
    ERROR_VARIABLE add_error)
if(NOT add_result EQUAL 0)
    message(FATAL_ERROR "Could not stage test fixture:\n${add_error}")
endif()
execute_process(
    COMMAND "${GIT_EXECUTABLE}" -C "${repository}"
        -c user.name=laiue-ci
        -c user.email=laiue-ci.invalid
        -c commit.gpgSign=false
        -c "core.hooksPath=${TEST_ROOT}/hooks"
        commit --quiet -m baseline
    RESULT_VARIABLE commit_result
    ERROR_VARIABLE commit_error)
if(NOT commit_result EQUAL 0)
    message(FATAL_ERROR "Could not commit test fixture:\n${commit_error}")
endif()

# Regression for GitHub Actions container jobs: the guard must use SOURCE_ROOT
# explicitly and must not depend on the shell's current working directory.
execute_process(
    COMMAND "${CMAKE_COMMAND}"
        "-DSOURCE_ROOT=${repository}"
        -P "${VERIFY_SCRIPT}"
    WORKING_DIRECTORY "${TEST_ROOT}/outside"
    RESULT_VARIABLE clean_result
    OUTPUT_VARIABLE clean_output
    ERROR_VARIABLE clean_error)
if(NOT clean_result EQUAL 0)
    message(FATAL_ERROR
        "Clean repository was rejected from an unrelated working "
        "directory:\n${clean_output}${clean_error}")
endif()

file(APPEND "${tracked_file}" "changed-by-build")
execute_process(
    COMMAND "${CMAKE_COMMAND}"
        "-DSOURCE_ROOT=${repository}"
        -P "${VERIFY_SCRIPT}"
    WORKING_DIRECTORY "${TEST_ROOT}/outside"
    RESULT_VARIABLE dirty_result
    OUTPUT_VARIABLE dirty_output
    ERROR_VARIABLE dirty_error)
if(dirty_result EQUAL 0)
    message(FATAL_ERROR "Modified tracked source was not rejected")
endif()
set(dirty_diagnostic "${dirty_output}${dirty_error}")
if(NOT dirty_diagnostic MATCHES "Build changed the checked-out source tree" OR
   NOT dirty_diagnostic MATCHES "tracked.txt")
    message(FATAL_ERROR
        "Dirty-tree diagnostic did not identify the failure:\n"
        "${dirty_diagnostic}")
endif()

file(WRITE "${tracked_file}" "checked in\n")
file(WRITE "${repository}/untracked.txt" "created-by-build\n")
execute_process(
    COMMAND "${CMAKE_COMMAND}"
        "-DSOURCE_ROOT=${repository}"
        -P "${VERIFY_SCRIPT}"
    WORKING_DIRECTORY "${TEST_ROOT}/outside"
    RESULT_VARIABLE untracked_result
    OUTPUT_VARIABLE untracked_output
    ERROR_VARIABLE untracked_error)
if(untracked_result EQUAL 0)
    message(FATAL_ERROR "Untracked build output was not rejected")
endif()
set(untracked_diagnostic "${untracked_output}${untracked_error}")
if(NOT untracked_diagnostic MATCHES
       "Build changed the checked-out source tree" OR
   NOT untracked_diagnostic MATCHES "untracked.txt")
    message(FATAL_ERROR
        "Untracked-file diagnostic did not identify the failure:\n"
        "${untracked_diagnostic}")
endif()
