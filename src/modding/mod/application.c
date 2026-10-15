#include "mod/application.h"

#include "platform/system.h"

uint32_t LaiueApplicationExecutableDirectory(wchar_t *output, uint32_t capacity)
{
    if (output == NULL || capacity == 0u)
        return 0u;
    if (!PlatformExecutableDirectory(output, capacity))
    {
        output[0] = L'\0';
        return 0u;
    }
    return 1u;
}

double LaiueApplicationMonotonicSeconds(void)
{
    return PlatformMonotonicSeconds();
}

void LaiueApplicationSleepMilliseconds(uint32_t milliseconds)
{
    PlatformSleepMilliseconds(milliseconds);
}

uint32_t LaiueApplicationInstallTerminationHandler(void)
{
    return PlatformInstallTerminationHandler() ? 1u : 0u;
}

void LaiueApplicationRemoveTerminationHandler(void)
{
    PlatformRemoveTerminationHandler();
}

uint32_t LaiueApplicationTerminationRequested(void)
{
    return PlatformTerminationRequested() ? 1u : 0u;
}
