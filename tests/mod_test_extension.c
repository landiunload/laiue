#include "mod/mod_api.h"
#include "mod_test_service.h"

#include <stddef.h>

static LaiueModTestCounterService *g_counter;

static void LAIUE_MOD_CALL TestExtensionUnload(void *modContext)
{
    LaiueModTestCounterService *counter = modContext;
    if (counter != NULL)
    {
        ++counter->unloadCount;
    }
    g_counter = NULL;
}

LAIUE_MOD_EXPORT LaiueModResult LAIUE_MOD_CALL LaiueModLoadV1(const LaiueModHostApiV1 *host,
                                                              LaiueModExportsV1 *exports)
{
    if (host == NULL || exports == NULL ||
        host->structSize < offsetof(LaiueModHostApiV1, reserved) ||
        host->abiVersion != LAIUE_MOD_ABI_VERSION_1 ||
        exports->structSize < offsetof(LaiueModExportsV1, reserved) ||
        exports->abiVersion != LAIUE_MOD_ABI_VERSION_1)
    {
        return -1;
    }

    uint32_t version = 0;
    uint32_t size = 0;
    g_counter = (LaiueModTestCounterService *)host->queryService(
        host->hostContext, LAIUE_MOD_TEST_COUNTER_SERVICE_NAME,
        LAIUE_MOD_TEST_COUNTER_SERVICE_VERSION, sizeof(*g_counter), &version, &size);
    if (g_counter == NULL || version != LAIUE_MOD_TEST_COUNTER_SERVICE_VERSION ||
        size < sizeof(*g_counter))
    {
        return -17;
    }

    ++g_counter->loadCount;
    host->log(host->hostContext, LAIUE_MOD_LOG_INFORMATION, "test extension initialized");
    exports->modContext = g_counter;
    exports->unload = TestExtensionUnload;
    return LAIUE_MOD_RESULT_OK;
}
