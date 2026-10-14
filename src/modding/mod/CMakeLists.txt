laiue_add_module(mod
    RUNTIME_ROLE CORE
    SOURCES
        application.c
        application.h
        mod.h
        mod_api.h
        mod_host.c
        mod_host.h
        module_api.h
        module_artifact.c
        module_manifest.c
        module_manifest.h
        module_profile.c
        module_profile.h
        module_host.c
        module_host.h
        mod_internal.c
        mod_internal.h
        mod_manifest.c
        mod_manifest.h
        mod_types.h
        ../../api.h
    POSIX_LINK
        ${CMAKE_DL_LIBS}
)
laiue_define_module_artifact_names(laiue_mod)
if(LAIUE_NATIVE_MODS_ENABLED)
    target_compile_definitions(laiue_mod PRIVATE LAIUE_NATIVE_MODS_ENABLED=1)
else()
    target_compile_definitions(laiue_mod PRIVATE LAIUE_NATIVE_MODS_ENABLED=0)
endif()
