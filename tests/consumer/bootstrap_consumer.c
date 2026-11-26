#include "mod/application.h"
#include "mod/module_host.h"
#include "numeric/numeric_service.h"

#include <stdint.h>

/* Exercises the installed SDK from an ordinary console application. */
int main(void)
{
    wchar_t directory[LAIUE_APPLICATION_PATH_CAPACITY];
    wchar_t fileName[LAIUE_MODULE_ARTIFACT_NAME_CAPACITY];
    if (LaiueApplicationExecutableDirectory(directory, LAIUE_APPLICATION_PATH_CAPACITY) == 0u ||
        LaiueModuleArtifactFileNameV1("numeric", fileName, LAIUE_MODULE_ARTIFACT_NAME_CAPACITY) !=
            LAIUE_MODULE_OK)
        return 1;
    const double started = LaiueApplicationMonotonicSeconds();

    LaiueModuleHostConfigV1 config;
    LaiueModuleHostConfigInitialize(&config);
    LaiueModuleDiagnostic diagnostic;
    LaiueModuleHost *host = LaiueModuleHostCreate(&config, &diagnostic);
    if (host == NULL)
        return 2;

    static const char *const names[] = {"numeric"};
    LaiueModuleLoadReportEntryV1 entries[1];
    LaiueModuleLoadReportV1 report;
    LaiueModuleLoadReportInitialize(&report, entries, 1u);
    const LaiueModuleNamedProfileV1 profile = {
        .structSize = sizeof(profile),
        .names = names,
        .nameCount = 1u,
    };
    const LaiueModuleStatus status =
        LaiueModuleHostLoadNamedProfileV1(host, &profile, &report, &diagnostic);
    int result = 0;
    if (status != LAIUE_MODULE_OK || report.loadedCount != 1u ||
        LaiueModuleHostIsLoaded(host, "laiue.numeric") == 0u)
        result = 3;
    else
    {
        const LaiueNumericServiceV1 *numeric = LaiueModuleHostQueryService(
            host, LAIUE_NUMERIC_SERVICE_NAME, LAIUE_NUMERIC_SERVICE_ABI_VERSION_1,
            sizeof(LaiueNumericServiceV1), NULL, NULL);
        if (numeric == NULL || numeric->structSize < sizeof(*numeric) ||
            numeric->abiVersion != LAIUE_NUMERIC_SERVICE_ABI_VERSION_1 || numeric->init == NULL ||
            numeric->destroy == NULL || numeric->tryAddInt64InPlace == NULL ||
            numeric->compareAddInt64ToInt64 == NULL)
            result = 4;
        else
        {
            InfiniteCoord value;
            numeric->init(&value);
            if (numeric->tryAddInt64InPlace(&value, -17) == 0u ||
                numeric->compareAddInt64ToInt64(&value, 0, -17) != 0)
                result = 5;
            numeric->destroy(&value);
        }
    }
    if (result == 0 && LaiueApplicationMonotonicSeconds() < started)
        result = 6;
    LaiueModuleHostUnloadAll(host);
    LaiueModuleHostDestroy(host);
    return result;
}
