laiue_add_program(laiue_viewer viewer.c
    MODULES window input ui graphics_d3d12 graphics_vulkan
    WINDOWED
    ANDROID_PACKAGE dev.laiue.viewer
    ANDROID_LABEL "laiue viewer")
target_link_libraries(laiue_viewer PRIVATE laiue::media_support)
