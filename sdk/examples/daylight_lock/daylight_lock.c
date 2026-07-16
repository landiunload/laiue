/*
 * Daylight Lock — вечное утро: время суток удерживается на 10:30.
 *
 * Демонстрирует кадровый хук: перезапись времени каждый кадр надёжнее
 * любых флагов — выключил мод, и солнце снова пошло.
 *
 *     cl /nologo /W4 /O2 /utf-8 /LD /I..\.. daylight_lock.c /Fe:daylight_lock.dll
 */

#include "laiue_mod_api.h"

#define LOCKED_HOUR 10.5f

static const LaiueModApi* g_api;

static void OnFrame(void* user, float deltaSeconds)
{
    (void)user;
    (void)deltaSeconds;
    g_api->setTimeHours(g_api->host, LOCKED_HOUR);
}

__declspec(dllexport) int32_t LaiueModInit(const LaiueModApi* api)
{
    g_api = api;
    api->setTimeHours(api->host, LOCKED_HOUR);
    api->setFrameCallback(api->host, OnFrame, 0);
    api->log(api->host, L"время зафиксировано на 10:30");
    return 0;
}

__declspec(dllexport) void LaiueModShutdown(void)
{
    g_api = 0;
}
