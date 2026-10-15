# Fails when a static archive contains any of the given objects. The bootstrap
# archive is linked by applications that bring their own C runtime, so the
# engine's no-CRT replacements of memcpy and friends must stay out of it.
# Run with -DARCHIVER=<lib.exe or llvm-lib> -DARCHIVE=<.lib>
# -DFORBIDDEN=<object paths separated by '|'>.
foreach(required IN ITEMS ARCHIVER ARCHIVE FORBIDDEN)
    if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR "${required} is required")
    endif()
endforeach()

execute_process(
    COMMAND "${ARCHIVER}" /NOLOGO /LIST "${ARCHIVE}"
    RESULT_VARIABLE list_result
    OUTPUT_VARIABLE members
    ERROR_VARIABLE list_error)
if(NOT list_result EQUAL 0)
    message(FATAL_ERROR "Could not list ${ARCHIVE}: ${list_error}")
endif()
string(REPLACE "\r\n" "\n" members "${members}")
string(REPLACE "\\" "/" members "${members}")
string(REPLACE "\n" ";" members "${members}")

string(REPLACE "|" ";" forbidden "${FORBIDDEN}")
foreach(object IN LISTS forbidden)
    get_filename_component(object_name "${object}" NAME)
    foreach(member IN LISTS members)
        get_filename_component(member_name "${member}" NAME)
        if(member_name STREQUAL object_name)
            message(FATAL_ERROR "${ARCHIVE} contains ${object_name}")
        endif()
    endforeach()
endforeach()
