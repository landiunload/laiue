add_executable(laiue_walk walk.c)
target_link_libraries(laiue_walk PRIVATE laiue_bootstrap)
target_compile_definitions(laiue_walk PRIVATE LAIUE_STATIC=1)
target_include_directories(laiue_walk PRIVATE
    "${PROJECT_SOURCE_DIR}/src")
laiue_configure_standalone_executable(laiue_walk WalkExampleEntryPoint)
