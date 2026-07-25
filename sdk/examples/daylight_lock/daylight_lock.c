/*
 * Daylight Lock — вечное утро: время суток удерживается на 10:30.
 *
 * Демонстрирует кадровый хук: перезапись времени каждый кадр надёжнее
 * любых флагов — выключил мод, и солнце снова пошло.
 *
 * Windows и Linux-команды сборки приведены в docs/modding.md.
 */

#include "laiue_mod_api.h"

#define LOCKED_HOUR 10.5f

static const LaiueModApi *g_api;

static void OnFrame(void *user, float deltaSeconds)
{
    (void)user;
    (void)deltaSeconds;
    g_api->setTimeHours(g_api->host, LOCKED_HOUR);
}

LAIUE_MOD_EXPORT int32_t LAIUE_MOD_CALL LaiueModInit(const LaiueModApi *api)
{
    g_api = api;
    api->setTimeHours(api->host, LOCKED_HOUR);
    api->setFrameCallback(api->host, OnFrame, 0);
    api->log(api->host, L"время зафиксировано на 10:30");
    return 0;
}

LAIUE_MOD_EXPORT void LAIUE_MOD_CALL LaiueModShutdown(void)
{
    g_api = 0;
}
