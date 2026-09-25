// Ручной benchmark построения кадрового описания панорамы. В ALL не входит
// и в CTest не регистрируется: его запускают осознанно и читают глазами.
//
// Меряются два разных пути PanoramaBuildFrameSetup:
//  * frame.*   — кеш прогрет, параметры не меняются: цена, которую платит
//    каждая страница кадра (сборка view * faceBasis и viewProjection проходов);
//  * rebuild.* — размер окна меняется каждый вызов, поэтому кеш инвалидируется
//    и отрабатывает ComputeFaceBounds + FaceRectProjection.
//
// Каждый отчёт содержит FNV-хеш байт полученного RendererFrameSetup. Он
// печатается, чтобы A/B-скрипт мог сравнить baseline и candidate побайтово:
// хеши обязаны совпадать, иначе изменение геометрии уже не эквивалентно.

#include "scene/panorama.h"
#include "platform/system.h"
#include "test_runtime.h"

#include <stdbool.h>
#include <stdint.h>

#define FRAME_ITERS 400000u
#define REBUILD_ITERS 400u
#define REPEATS 5u

static volatile uint64_t panoramaSink;

static const float VIEW[16] = {
    0.7986355f, 0.0f,      -0.6018150f, 0.0f,
    0.0f,       1.0f,       0.0f,       0.0f,
    0.6018150f, 0.0f,       0.7986355f, 0.0f,
    123.0f,     64.0f,     -250.0f,     1.0f,
};

static void WriteText(const char *text)
{
    LaiueTestRuntimeWrite(text);
}

static void WriteUnsigned(uint64_t value)
{
    char digits[21];
    uint32_t length = 0u;
    if (value == 0u)
    {
        digits[length++] = '0';
    }
    while (value != 0u)
    {
        digits[length++] = (char)('0' + (value % 10u));
        value /= 10u;
    }
    char text[22];
    for (uint32_t index = 0u; index < length; ++index)
    {
        text[index] = digits[length - index - 1u];
    }
    text[length] = '\0';
    WriteText(text);
}

static void WriteHex(uint64_t value)
{
    static const char digits[] = "0123456789abcdef";
    char text[17];
    for (uint32_t index = 0u; index < 16u; ++index)
    {
        text[index] = digits[(value >> ((15u - index) * 4u)) & 0xfu];
    }
    text[16] = '\0';
    WriteText(text);
}

// Наносекунды на вызов с четырьмя знаками — разница здесь может быть
// единицами наносекунд, поэтому миллисекунды непригодны.
static void WriteNanoseconds(double seconds, uint64_t operations)
{
    if (operations == 0u || !(seconds > 0.0))
    {
        WriteText("0.0000");
        return;
    }
    double value = seconds * 1000000000.0 / (double)operations;
    if (value > 1000000.0)
    {
        value = 1000000.0;
    }
    uint64_t whole = (uint64_t)value;
    WriteUnsigned(whole);
    WriteText(".");
    uint64_t fraction = (uint64_t)((value - (double)whole) * 10000.0);
    if (fraction > 9999u)
    {
        fraction = 9999u;
    }
    if (fraction < 1000u) WriteText("0");
    if (fraction < 100u) WriteText("0");
    if (fraction < 10u) WriteText("0");
    WriteUnsigned(fraction);
}

static uint64_t HashBytes(uint64_t hash, const void *data, uint32_t count)
{
    const unsigned char *bytes = (const unsigned char *)data;
    for (uint32_t index = 0u; index < count; ++index)
    {
        hash ^= (uint64_t)bytes[index];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static uint64_t HashSetup(const RendererFrameSetup *setup)
{
    return HashBytes(UINT64_C(14695981039346656037), setup,
        (uint32_t)sizeof(*setup));
}

static void ClearCache(PanoramaCache *cache)
{
    unsigned char *bytes = (unsigned char *)cache;
    for (uint32_t index = 0u; index < sizeof(*cache); ++index)
    {
        bytes[index] = 0u;
    }
}

static void Report(const char *name, double seconds, uint64_t operations,
    uint64_t checksum)
{
    WriteText("panorama ");
    WriteText(name);
    WriteText(" ops=");
    WriteUnsigned(operations);
    WriteText(" ns_per_op=");
    WriteNanoseconds(seconds, operations);
    WriteText(" checksum=");
    WriteHex(checksum);
    WriteText("\n");
}

// Прогретый кеш: параметры кадра не меняются, пересборки нет.
static uint64_t MeasureFrame(const char *name, RenderProjection projection,
    float fov, int32_t width, int32_t height)
{
    PanoramaCache cache;
    ClearCache(&cache);
    RendererFrameSetup setup;
    PanoramaBuildFrameSetup(&cache, projection, fov, width, height,
        0.05f, 4096.0f, VIEW, &setup);
    uint64_t checksum = HashSetup(&setup);

    double best = 0.0;
    for (uint32_t repeat = 0u; repeat < REPEATS; ++repeat)
    {
        double begin = PlatformMonotonicSeconds();
        for (uint32_t index = 0u; index < FRAME_ITERS; ++index)
        {
            PanoramaBuildFrameSetup(&cache, projection, fov, width, height,
                0.05f, 4096.0f, VIEW, &setup);
        }
        double elapsed = PlatformMonotonicSeconds() - begin;
        if (repeat == 0u || elapsed < best)
        {
            best = elapsed;
        }
        checksum = HashSetup(&setup) ^ (checksum << 1);
    }

    panoramaSink += checksum;
    Report(name, best, FRAME_ITERS, checksum);
    return checksum;
}

// Кеш инвалидируется каждый вызов сменой ширины на соседнюю.
static uint64_t MeasureRebuild(const char *name, RenderProjection projection,
    float fov)
{
    PanoramaCache cache;
    ClearCache(&cache);
    RendererFrameSetup setup;
    PanoramaBuildFrameSetup(&cache, projection, fov, 1920, 1080,
        0.05f, 4096.0f, VIEW, &setup);
    uint64_t checksum = HashSetup(&setup);

    double best = 0.0;
    for (uint32_t repeat = 0u; repeat < REPEATS; ++repeat)
    {
        double begin = PlatformMonotonicSeconds();
        for (uint32_t index = 0u; index < REBUILD_ITERS; ++index)
        {
            int32_t width = (index & 1u) != 0u ? 1920 : 1921;
            PanoramaBuildFrameSetup(&cache, projection, fov, width, 1080,
                0.05f, 4096.0f, VIEW, &setup);
        }
        double elapsed = PlatformMonotonicSeconds() - begin;
        if (repeat == 0u || elapsed < best)
        {
            best = elapsed;
        }
        checksum = HashSetup(&setup) ^ (checksum << 1);
    }

    panoramaSink += checksum;
    Report(name, best, REBUILD_ITERS, checksum);
    return checksum;
}

// Широкая проверка эквивалентности: все режимы проекции, граничные поля
// зрения (1, 120 — порог авто, 170 — предел перспективы, 180, 360) и
// размеры, включая вырожденный 1x1. Хеш печатается целиком, чтобы
// baseline и candidate сравнивались побайтово.
static void DumpConfig(const char *name, RenderProjection projection, float fov,
    int32_t width, int32_t height)
{
    PanoramaCache cache;
    ClearCache(&cache);
    RendererFrameSetup setup;
    PanoramaBuildFrameSetup(&cache, projection, fov, width, height,
        0.05f, 4096.0f, VIEW, &setup);
    WriteText("dump ");
    WriteText(name);
    WriteText(" panorama=");
    WriteUnsigned(setup.panorama ? 1u : 0u);
    WriteText(" passes=");
    WriteUnsigned(setup.passCount);
    WriteText(" faceRes=");
    WriteUnsigned(setup.faceResolution);
    for (uint32_t slot = 0u; slot < setup.passCount; ++slot)
    {
        const RendererScenePass *pass = &setup.passes[slot];
        WriteText(" f");
        WriteUnsigned(pass->faceIndex);
        WriteText("=");
        WriteUnsigned(pass->rectMinX);
        WriteText(",");
        WriteUnsigned(pass->rectMinY);
        WriteText(",");
        WriteUnsigned(pass->rectMaxX);
        WriteText(",");
        WriteUnsigned(pass->rectMaxY);
    }
    WriteText(" rect=");
    WriteHex(HashBytes(UINT64_C(14695981039346656037),
        cache.rect, (uint32_t)sizeof(cache.rect)));
    // Хеш проекции первого прохода: под /fp:fast он вправе отличаться от
    // прежней сборки на пару ulp при том же прямоугольнике. Прямоугольники
    // выше — то, что определяет растеризуемую область, и они обязаны
    // совпадать побайтово.
    WriteText(" vp=");
    WriteHex(HashBytes(UINT64_C(14695981039346656037),
        setup.passes[0].viewProjection, 64u));
    WriteText(" idx=");
    WriteHex(HashBytes(UINT64_C(14695981039346656037),
        cache.faceIndex, (uint32_t)sizeof(cache.faceIndex)));
    WriteText(" hdr=");
    WriteHex(HashBytes(UINT64_C(14695981039346656037),
        &cache, 32u));
    WriteText("\n");
}

static uint64_t VerifyBattery(void)
{
    static const RenderProjection projections[4] = {
        RENDER_PROJECTION_AUTO, RENDER_PROJECTION_PERSPECTIVE,
        RENDER_PROJECTION_FISHEYE, RENDER_PROJECTION_CYLINDER,
    };
    static const float fovs[8] = {
        1.0f, 45.0f, 90.0f, 120.0f, 150.0f, 170.0f, 180.0f, 360.0f,
    };
    static const int32_t sizes[6][2] = {
        { 1, 1 }, { 16, 9 }, { 64, 64 },
        { 640, 480 }, { 1920, 1080 }, { 3840, 2160 },
    };

    uint64_t total = UINT64_C(14695981039346656037);
    PanoramaCache cache;
    ClearCache(&cache);
    RendererFrameSetup setup;

    for (uint32_t p = 0u; p < 4u; ++p)
    {
        for (uint32_t f = 0u; f < 8u; ++f)
        {
            for (uint32_t s = 0u; s < 6u; ++s)
            {
                // Два одинаковых вызова: второй обязан переиспользовать кеш
                // и выдать побайтово то же описание кадра.
                PanoramaBuildFrameSetup(&cache, projections[p], fovs[f],
                    sizes[s][0], sizes[s][1], 0.05f, 4096.0f, VIEW, &setup);
                total = HashSetup(&setup) ^ (total << 1);

                PanoramaBuildFrameSetup(&cache, projections[p], fovs[f],
                    sizes[s][0], sizes[s][1], 0.05f, 4096.0f, VIEW, &setup);
                total = HashSetup(&setup) ^ (total << 1);
            }
        }
    }
    return total;
}

LAIUE_TEST_ENTRY(PanoramaBenchmarkEntryPoint)
{
    WriteText("laiue panorama benchmark\n");

    uint64_t battery = VerifyBattery();
    WriteText("panorama verify checksum=");
    WriteHex(battery);
    WriteText("\n");

    MeasureFrame("frame.fisheye", RENDER_PROJECTION_FISHEYE, 170.0f, 1920, 1080);
    MeasureFrame("frame.cylinder", RENDER_PROJECTION_CYLINDER, 170.0f, 1920, 1080);
    MeasureFrame("frame.perspective", RENDER_PROJECTION_PERSPECTIVE, 90.0f, 1920, 1080);
    MeasureRebuild("rebuild.fisheye", RENDER_PROJECTION_FISHEYE, 170.0f);
    MeasureRebuild("rebuild.cylinder", RENDER_PROJECTION_CYLINDER, 170.0f);

    DumpConfig("fisheye170", RENDER_PROJECTION_FISHEYE, 170.0f, 1920, 1080);
    DumpConfig("cylinder170", RENDER_PROJECTION_CYLINDER, 170.0f, 1920, 1080);
    DumpConfig("fisheye270", RENDER_PROJECTION_FISHEYE, 270.0f, 3840, 2160);
    DumpConfig("cylinder90", RENDER_PROJECTION_CYLINDER, 90.0f, 1280, 720);
    DumpConfig("fisheye360", RENDER_PROJECTION_FISHEYE, 360.0f, 1920, 1080);
    DumpConfig("fisheye130", RENDER_PROJECTION_FISHEYE, 130.0f, 1920, 1080);
    DumpConfig("cylinder180", RENDER_PROJECTION_CYLINDER, 180.0f, 1920, 1080);
    DumpConfig("fisheye1", RENDER_PROJECTION_FISHEYE, 1.0f, 1920, 1080);

    WriteText("panorama benchmark done sink=");
    WriteUnsigned(panoramaSink != 0u ? 1u : 0u);
    WriteText("\n");
    LAIUE_TEST_SUCCESS();
}
