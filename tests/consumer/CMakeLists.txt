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
