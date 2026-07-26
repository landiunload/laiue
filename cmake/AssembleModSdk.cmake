foreach(required IN ITEMS
        SOURCE_ROOT WINDOWS_STAGE GNU_STAGE MUSL_STAGE OUTPUT_DIRECTORY)
    if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR "Missing ${required}")
    endif()
endforeach()

file(GLOB windows_packages
    RELATIVE "${WINDOWS_STAGE}" "${WINDOWS_STAGE}/*.lmp")
file(GLOB gnu_packages
    RELATIVE "${GNU_STAGE}" "${GNU_STAGE}/*.lmp")
file(GLOB musl_packages
    RELATIVE "${MUSL_STAGE}" "${MUSL_STAGE}/*.lmp")
list(SORT windows_packages)
list(SORT gnu_packages)
list(SORT musl_packages)
if(NOT windows_packages OR
   NOT "${windows_packages}" STREQUAL "${gnu_packages}" OR
   NOT "${windows_packages}" STREQUAL "${musl_packages}")
    message(FATAL_ERROR
        "Windows/GNU/musl ModSDK stages have different package sets:\n"
        "  Windows: ${windows_packages}\n"
        "  GNU: ${gnu_packages}\n"
        "  musl: ${musl_packages}")
endif()

file(MAKE_DIRECTORY
    "${OUTPUT_DIRECTORY}/sdk"
    "${OUTPUT_DIRECTORY}/mods"
    "${OUTPUT_DIRECTORY}/docs")
file(COPY "${SOURCE_ROOT}/sdk/"
    DESTINATION "${OUTPUT_DIRECTORY}/sdk")
file(COPY_FILE "${SOURCE_ROOT}/LICENSE"
    "${OUTPUT_DIRECTORY}/LICENSE" ONLY_IF_DIFFERENT)
file(COPY_FILE "${SOURCE_ROOT}/docs/modding.md"
    "${OUTPUT_DIRECTORY}/docs/modding.md" ONLY_IF_DIFFERENT)

foreach(package_name IN LISTS windows_packages)
    if(NOT package_name MATCHES "^([a-z0-9_]+)\\.lmp$")
        message(FATAL_ERROR
            "Invalid ModSDK package directory: ${package_name}")
    endif()
    set(mod_name "${CMAKE_MATCH_1}")
    set(windows_name "${mod_name}.windows-x86_64.dll")
    set(gnu_name "${mod_name}.linux-x86_64-gnu.so")
    set(musl_name "${mod_name}.linux-x86_64-musl.so")
    set(windows_file
        "${WINDOWS_STAGE}/${mod_name}.lmp/${windows_name}")
    set(gnu_file
        "${GNU_STAGE}/${mod_name}.lmp/${gnu_name}")
    set(musl_file
        "${MUSL_STAGE}/${mod_name}.lmp/${musl_name}")
    foreach(artifact IN ITEMS
            "${windows_file}" "${gnu_file}" "${musl_file}")
        if(NOT EXISTS "${artifact}" OR IS_DIRECTORY "${artifact}")
            message(FATAL_ERROR
                "Missing platform artifact for ${mod_name}: ${artifact}")
        endif()
        file(SIZE "${artifact}" artifact_size)
        if(artifact_size EQUAL 0)
            message(FATAL_ERROR
                "Empty platform artifact for ${mod_name}: ${artifact}")
        endif()
    endforeach()

    set(package_directory
        "${OUTPUT_DIRECTORY}/mods/${mod_name}.lmp")
    file(MAKE_DIRECTORY "${package_directory}")
    file(COPY_FILE "${windows_file}"
        "${package_directory}/${windows_name}" ONLY_IF_DIFFERENT)
    file(COPY_FILE "${gnu_file}"
        "${package_directory}/${gnu_name}" ONLY_IF_DIFFERENT)
    file(COPY_FILE "${musl_file}"
        "${package_directory}/${musl_name}" ONLY_IF_DIFFERENT)

    file(READ
        "${SOURCE_ROOT}/sdk/examples/${mod_name}/mod.lm.in"
        manifest)
    string(CONCAT native_entries
        "entry_windows_x86_64 = ${windows_name}\n"
        "entry_linux_x86_64_gnu = ${gnu_name}\n"
        "entry_linux_x86_64_musl = ${musl_name}")
    string(REPLACE "@LAIUE_MOD_NATIVE_ENTRIES@"
        "${native_entries}" manifest "${manifest}")
    if(manifest MATCHES "@LAIUE_MOD_NATIVE_ENTRIES@")
        message(FATAL_ERROR
            "Manifest placeholder remained for ${mod_name}")
    endif()
    file(WRITE "${package_directory}/mod.lm" "${manifest}")
endforeach()
