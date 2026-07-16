/*
 * Auto Bridge — пример нативного DLL-мода laiue.
 *
 * В режиме ходьбы, пока игрок не стоит на земле, мод подкладывает блок
 * земли под ноги: шагаешь с обрыва — мост строится сам.
 *
 * Мод не линкуется с игрой: достаточно sdk/laiue_mod_api.h.
 * Сборка из Developer Command Prompt x64 (см. build.bat):
 *
 *     cl /nologo /W4 /O2 /LD /I..\.. auto_bridge.c /Fe:auto_bridge.dll
 */

#include "laiue_mod_api.h"

static const LaiueModApi* g_api;

static int64_t FloorToInt64(double value)
{
    int64_t truncated = (int64_t)value;
    return (double)truncated > value ? truncated - 1 : truncated;
}

static void OnFrame(void* user, float deltaSeconds)
{
    (void)user;
    (void)deltaSeconds;

    if (g_api->getGameMode(g_api->host) != LAIUE_GAME_MODE_WALK
        || g_api->isPlayerGrounded(g_api->host))
    {
        return;
    }

    double eye[3];
    g_api->getPlayerPosition(g_api->host, eye);

    /* Глаза стоящего игрока — ~1.75 блока над ступнями; блок моста
       кладётся сразу под ноги. */
    int64_t x = FloorToInt64(eye[0]);
    int64_t y = FloorToInt64(eye[1]);
    int64_t z = FloorToInt64(eye[2] - 1.75 - 0.35);

    if (g_api->getBlock(g_api->host, x, y, z) == LAIUE_BLOCK_AIR)
    {
        g_api->setBlock(g_api->host, x, y, z, LAIUE_BLOCK_EARTH);
    }
}

__declspec(dllexport) int32_t LaiueModInit(const LaiueModApi* api)
{
    /* Хост может быть новее: доступные поля ограничены structSize. */
    if (api->apiVersion < 1u)
    {
        return 1;
    }

    g_api = api;
    api->setFrameCallback(api->host, OnFrame, 0);
    api->log(api->host, L"Auto Bridge готов: шагайте с обрыва (режим G)");
    return 0;
}

__declspec(dllexport) void LaiueModShutdown(void)
{
    g_api = 0;
}
