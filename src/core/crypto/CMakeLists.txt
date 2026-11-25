# Криптография движка: SHA-512, подписи Ed25519 и общая с X25519 арифметика
# поля 2^255 - 19. Статическая, как math_support: модулям она нужна внутри,
# но публичным ABI движка не является и в SDK не устанавливается.
add_library(laiue_crypto_support STATIC
    ed25519.c
    ed25519.h
    field25519.h
    scalar25519.c
    scalar25519.h
    sha512.c
    sha512.h
    wide.h
    wipe.h)
target_link_libraries(laiue_crypto_support
    PRIVATE laiue::build_options)
target_include_directories(laiue_crypto_support
    PUBLIC "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src/core>")
set_target_properties(laiue_crypto_support PROPERTIES
    POSITION_INDEPENDENT_CODE ON)
if(NOT MSVC)
    set_target_properties(laiue_crypto_support PROPERTIES
        C_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN YES)
endif()
add_library(laiue::crypto_support ALIAS laiue_crypto_support)
