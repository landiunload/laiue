laiue_add_module(audio
    RUNTIME_ROLE GRAPHICS
    SOURCES
        audio_windows.c
        audio.h
        ../api.h
    LINK
        mf
        mfplat
        mfuuid
        ole32
        oleaut32
        shlwapi
        uuid
)
