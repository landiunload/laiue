if(WIN32)
    set(laiue_platform_system_source system_windows.c)
else()
    set(laiue_platform_system_source system_posix.c)
    find_package(Threads REQUIRED)
    find_package(OpenSSL 3.0 REQUIRED COMPONENTS Crypto)
endif()

# Headless OS boundary. It is static so internal platform symbols do not
# become part of the engine DLL/SO ABI; every shared module still consumes
# the exact same implementation target and compile contract.
add_library(laiue_platform_support STATIC
    ${laiue_platform_system_source}
    system.h)
add_library(laiue::platform_support ALIAS laiue_platform_support)
target_link_libraries(laiue_platform_support
    PRIVATE laiue::build_options)
set_target_properties(laiue_platform_support PROPERTIES
    POSITION_INDEPENDENT_CODE ON)

if(WIN32)
    target_link_libraries(laiue_platform_support
        PUBLIC kernel32 bcrypt)
else()
    target_link_libraries(laiue_platform_support
        PUBLIC Threads::Threads OpenSSL::Crypto ${CMAKE_DL_LIBS})
endif()

# Window/input/render stack is intentionally absent from Linux server builds.
if(LAIUE_BUILD_CLIENT)
    laiue_add_module(window
        SOURCES
            window.c
            window.h
            time.c
            time.h
            ../api.h
        WINDOWS_LINK
            user32
            gdi32)
endif()
