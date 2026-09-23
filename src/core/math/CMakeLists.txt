# Внутренняя граница математики. Статическая, как platform_support:
# скалярные функции нужны сцене, интерфейсу и звуку, но публичным ABI
# движка не являются и в SDK не устанавливаются.
add_library(laiue_math_support STATIC
    scalar.c
    scalar.h)
target_link_libraries(laiue_math_support
    PRIVATE laiue::build_options)
target_include_directories(laiue_math_support
    PUBLIC "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>")
set_target_properties(laiue_math_support PROPERTIES
    POSITION_INDEPENDENT_CODE ON)
if(NOT MSVC)
    set_target_properties(laiue_math_support PROPERTIES
        C_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN YES)
endif()
add_library(laiue::math_support ALIAS laiue_math_support)
