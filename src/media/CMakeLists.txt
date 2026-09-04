# Форматы содержимого: чтение чужих (PNG, GIF, JPEG, WAV, MP3) и
# запись своих (`.lt`, `.la`). Библиотека внутренняя и
# статическая — её символы не становятся частью ABI модулей, ровно как у
# platform_support и math_support.
#
# Ей пользуются и движок, и офлайн-инструменты: одна реализация вместо
# двух, и та, что проверена тестами, идёт в обе стороны.
add_library(laiue_media_support STATIC
    gif_decode.c
    gif_decode.h
    image.c
    image.h
    inflate.c
    inflate.h
    la_encode.c
    la_encode.h
    lt_encode.c
    lt_encode.h
    jpeg_decode.c
    jpeg_decode.h
    mp3_decode.c
    mp3_decode.h
    mp3_tables.h
    png_decode.c
    png_decode.h
    sound.c
    sound.h
    wave_decode.c
    wave_decode.h)
target_link_libraries(laiue_media_support PRIVATE laiue::build_options)
set_target_properties(laiue_media_support PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(LAIUE_PLATFORM_POSIX)
    set_target_properties(laiue_media_support PROPERTIES
        C_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN YES)
endif()
add_library(laiue::media_support ALIAS laiue_media_support)
