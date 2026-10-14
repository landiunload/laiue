#include "mod/mod_host.h"
#include "test_runtime.h"

LAIUE_TEST_ENTRY(ModHostDisabledTestEntry)
{
    LaiueModHostConfig config;
    LaiueModHostConfigInitialize(&config, L".");
    LaiueModDiagnostic diagnostic;
    LaiueModHost *host = LaiueModHostCreate(&config, &diagnostic);
    if (host == NULL)
    {
        LaiueTestRuntimeWrite("disabled native-mod host creation failed\n");
        LaiueTestRuntimeExit(1);
    }

    LaiueModStatus status = LaiueModHostLoad(host, L"fixture.lmp", NULL, &diagnostic);
    LaiueModHostDestroy(host);
    if (status != LAIUE_MOD_STATUS_PLATFORM_UNSUPPORTED ||
        diagnostic.status != LAIUE_MOD_STATUS_PLATFORM_UNSUPPORTED)
    {
        LaiueTestRuntimeWrite("disabled native mods did not fail as unsupported\n");
        LaiueTestRuntimeExit(1);
    }
    LAIUE_TEST_SUCCESS();
}
