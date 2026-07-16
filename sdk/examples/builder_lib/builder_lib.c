/*
 * Builder Lib — библиотечный мод: сам ничего не строит, но публикует
 * интерфейс BuilderLibV1 для других модов (см. builder_lib.h).
 *
 * Паттерн: библиотека держит статическую таблицу функций и публикует
 * её в LaiueModInit. Потребители должны стоять в mods/enabled.txt
 * ниже неё. При изменении состава модов хост перезагружает цепочку,
 * поэтому висячих указателей на таблицу не бывает.
 *
 *     cl /nologo /W4 /O2 /utf-8 /LD /I..\.. builder_lib.c /Fe:builder_lib.dll
 */

#include "builder_lib.h"
#include "laiue_mod_api.h"

static const LaiueModApi* g_api;

/* Синус без CRT: полином на [-pi, pi] — примеру этого достаточно. */
static float LibSin(float radians)
{
    while (radians > 3.14159265f) radians -= 6.28318531f;
    while (radians < -3.14159265f) radians += 6.28318531f;
    float x2 = radians * radians;
    float series = -2.50521084e-08f;
    series = series * x2 + 2.75573192e-06f;
    series = series * x2 - 1.98412698e-04f;
    series = series * x2 + 8.33333333e-03f;
    series = series * x2 - 1.66666667e-01f;
    series = series * x2 + 1.0f;
    return radians * series;
}

static float LibCos(float radians)
{
    return LibSin(radians + 1.57079633f);
}

static int64_t RoundToBlock(float value)
{
    return value >= 0.0f ? (int64_t)(value + 0.5f)
                         : -(int64_t)(0.5f - value);
}

static void FillBox(int64_t x0, int64_t y0, int64_t z0,
    int64_t x1, int64_t y1, int64_t z1, uint8_t block)
{
    if (x1 < x0) { int64_t t = x0; x0 = x1; x1 = t; }
    if (y1 < y0) { int64_t t = y0; y0 = y1; y1 = t; }
    if (z1 < z0) { int64_t t = z0; z0 = z1; z1 = t; }

    for (int64_t z = z0; z <= z1; ++z)
        for (int64_t y = y0; y <= y1; ++y)
            for (int64_t x = x0; x <= x1; ++x)
                g_api->setBlock(g_api->host, x, y, z, block);
}

static void BuildHelix(int64_t centerX, int64_t centerY, int64_t baseZ,
    int32_t radius, int32_t height, uint8_t block)
{
    for (int32_t step = 0; step < height * 2; ++step)
    {
        float angle = (float)step * 0.35f;
        float level = (float)step * 0.5f;
        int64_t x = centerX + RoundToBlock(LibCos(angle) * (float)radius);
        int64_t y = centerY + RoundToBlock(LibSin(angle) * (float)radius);
        g_api->setBlock(g_api->host, x, y, baseZ + (int64_t)level, block);
    }
}

static BuilderLibV1 g_builder = {
    BUILDER_LIB_VERSION,
    FillBox,
    BuildHelix,
};

__declspec(dllexport) int32_t LaiueModInit(const LaiueModApi* api)
{
    g_api = api;
    if (!api->publishInterface(api->host,
            BUILDER_LIB_NAME, BUILDER_LIB_VERSION, &g_builder))
    {
        api->log(api->host, L"не удалось опубликовать интерфейс");
        return 1;
    }
    api->log(api->host, L"интерфейс laiue.builder v1 опубликован");
    return 0;
}

__declspec(dllexport) void LaiueModShutdown(void)
{
    g_api = 0;
}
