#include "mod/module_host.h"

int main(void)
{
    LaiueModuleHostConfigV1 config;
    LaiueModuleHostConfigInitialize(&config);
    LaiueModuleDiagnostic diagnostic;
    LaiueModuleHost *host = LaiueModuleHostCreate(&config, &diagnostic);
    if (host == 0)
        return 1;
    LaiueModuleHostDestroy(host);
    return 0;
}
