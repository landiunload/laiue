# Внутреннее чтение шрифта: одна реализация для UI и тестов, без нового ABI.
add_library(laiue_ui_font_support STATIC
    truetype_reader.c
    truetype_reader.h)
target_link_libraries(laiue_ui_font_support PRIVATE laiue::build_options)
target_include_directories(laiue_ui_font_support
    PUBLIC "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>")
set_target_properties(laiue_ui_font_support PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(NOT MSVC)
    set_target_properties(laiue_ui_font_support PROPERTIES
        C_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN YES)
endif()
add_library(laiue::ui_font_support ALIAS laiue_ui_font_support)

laiue_add_module(ui
    RUNTIME_ROLE CORE
    SOURCES
        ui.c
        ui.h
        ui_service.h
        module_entry.c
        ui_font.c
        ui_font.h
        ui_font_internal.h
        ui_format.c
        ui_format.h
        ../api.h
    LINK
        laiue::math_support
    WINDOWS_LINK
        gdi32
)
