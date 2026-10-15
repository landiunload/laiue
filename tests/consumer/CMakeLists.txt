cmake_minimum_required(VERSION 4.2...4.4)
project(laiue_consumer LANGUAGES C)

# The installed-consumer test passes the engine's own version; a standalone
# configure accepts any compatible SDK.
find_package(laiue ${LAIUE_EXPECTED_VERSION} CONFIG REQUIRED)

add_executable(laiue_consumer consumer.c)
target_compile_features(laiue_consumer PRIVATE c_std_17)
target_link_libraries(laiue_consumer PRIVATE laiue::engine)
if(TARGET laiue::render)
    target_compile_definitions(laiue_consumer PRIVATE
        LAIUE_CONSUMER_HAS_GRAPHICS=1)
endif()

# The recommended application shape: only the static bootstrap is linked and
# every technology is loaded by name at run time.
add_executable(laiue_bootstrap_consumer bootstrap_consumer.c)
target_compile_features(laiue_bootstrap_consumer PRIVATE c_std_17)
target_link_libraries(laiue_bootstrap_consumer PRIVATE laiue::bootstrap)

# Every installed header must compile on its own with the SDK include root, as
# an application includes it. One generated source per header keeps a missing
# include from hiding behind an earlier one.
get_target_property(laiue_include_root laiue::bootstrap INTERFACE_INCLUDE_DIRECTORIES)
file(GLOB_RECURSE laiue_public_headers RELATIVE "${laiue_include_root}"
    "${laiue_include_root}/*.h")
if(NOT laiue_public_headers)
    message(FATAL_ERROR "No public headers under ${laiue_include_root}")
endif()
set(laiue_header_sources)
foreach(header IN LISTS laiue_public_headers)
    string(MAKE_C_IDENTIFIER "${header}" header_identifier)
    set(header_source "${CMAKE_CURRENT_BINARY_DIR}/headers/${header_identifier}.c")
    file(CONFIGURE OUTPUT "${header_source}"
        CONTENT "#include \"${header}\"\n\nint laiue_header_${header_identifier};\n"
        @ONLY NEWLINE_STYLE LF)
    list(APPEND laiue_header_sources "${header_source}")
endforeach()
add_library(laiue_public_headers OBJECT ${laiue_header_sources})
target_compile_features(laiue_public_headers PRIVATE c_std_17)
target_link_libraries(laiue_public_headers PRIVATE laiue::bootstrap)
add_dependencies(laiue_consumer laiue_public_headers)
