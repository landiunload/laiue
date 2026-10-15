#include "mod/application.h"
#include "mod/module_host.h"
#include "numeric/numeric_service.h"

#include <stdint.h>

/* Loads the numeric module by name the way an application would. Whether the
 * artifact sits next to this executable depends on the platform layout of the
 * installed SDK, so both a loaded and a skipped module are valid outcomes; a
 * loaded one must publish its service. */
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
        .flags = LAIUE_MODULE_PROFILE_ALLOW_PARTIAL,
        .names = names,
        .nameCount = 1u,
    };
    const LaiueModuleStatus status =
        LaiueModuleHostLoadNamedProfileV1(host, &profile, &report, &diagnostic);
    int result = 0;
    if (status != LAIUE_MODULE_OK && status != LAIUE_MODULE_PARTIAL)
        result = 3;
    else if (LaiueModuleHostIsLoaded(host, "laiue.numeric") != 0u &&
             LaiueModuleHostQueryService(host, LAIUE_NUMERIC_SERVICE_NAME, 1u, 1u, NULL, NULL) ==
                 NULL)
        result = 4;
    else if (LaiueApplicationMonotonicSeconds() < started)
        result = 5;
    LaiueModuleHostUnloadAll(host);
    LaiueModuleHostDestroy(host);
    return result;
}
