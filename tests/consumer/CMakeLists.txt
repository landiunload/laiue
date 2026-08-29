cmake_minimum_required(VERSION 3.28)
project(laiue_consumer LANGUAGES C)

find_package(laiue 0.6 CONFIG REQUIRED)

add_executable(laiue_consumer consumer.c)
target_compile_features(laiue_consumer PRIVATE c_std_17)
target_link_libraries(laiue_consumer PRIVATE laiue::engine)
