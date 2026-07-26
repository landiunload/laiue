if(NOT DEFINED SOURCE OR SOURCE STREQUAL "" OR
   NOT DEFINED DESTINATION OR DESTINATION STREQUAL "")
    message(FATAL_ERROR
        "CopySharedLibrary.cmake requires SOURCE and DESTINATION")
endif()
if(NOT EXISTS "${SOURCE}")
    message(FATAL_ERROR "Shared library does not exist: ${SOURCE}")
endif()

file(MAKE_DIRECTORY "${DESTINATION}")
file(COPY "${SOURCE}"
    DESTINATION "${DESTINATION}"
    FOLLOW_SYMLINK_CHAIN)
