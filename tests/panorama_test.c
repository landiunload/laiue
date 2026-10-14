// Проверка геометрии панорамы: инварианты прямоугольников и детерминизм
// кеша. Кеш обязан полностью пересчитываться при смене проекции, поля
// зрения или размера окна и переиспользоваться без изменений в остальных
// случаях, поэтому один и тот же набор параметров всегда даёт побайтово
// одинаковое RendererFrameSetup. Это регрессия на «лишний/пропущенный
// пересчёт» — цена ошибки здесь тихо испорченная картинка широкого угла.

#include "scene/panorama.h"
#include "scene/math.h"
#include "test_runtime.h"

#include <stdbool.h>
#include <stdint.h>

static uint32_t panoramaChecks;

static void TestMatrix4Multiply(const float left[16], const float right[16],
                                float out[16])
{
    Matrix4Multiply(left, right, out);
}

static const LaiueSceneMathServiceV1 sceneMath = {
    .structSize = sizeof(LaiueSceneMathServiceV1),
    .abiVersion = LAIUE_SCENE_MATH_SERVICE_ABI_VERSION_1,
    .matrix4Multiply = TestMatrix4Multiply,
};

static void PanoramaExpect(bool condition, const char *name)
{
    ++panoramaChecks;
    if (condition)
    {
        return;
    }
    LaiueTestRuntimeWrite("panorama check failed: ");
    LaiueTestRuntimeWrite(name);
    LaiueTestRuntimeWrite("\n");
    LaiueTestRuntimeExit(1);
}

static bool BytesEqual(const void *left, const void *right, uint32_t count)
{
    const unsigned char *a = (const unsigned char *)left;
    const unsigned char *b = (const unsigned char *)right;
    for (uint32_t index = 0u; index < count; ++index)
    {
        if (a[index] != b[index])
        {
            return false;
        }
    }
    return true;
}

static void ClearCache(PanoramaCache *cache)
{
    unsigned char *bytes = (unsigned char *)cache;
    for (uint32_t index = 0u; index < sizeof(*cache); ++index)
    {
        bytes[index] = 0u;
    }
}

static const float VIEW[16] = {
    0.7986355f, 0.0f,      -0.6018150f, 0.0f,
    0.0f,       1.0f,       0.0f,       0.0f,
    0.6018150f, 0.0f,       0.7986355f, 0.0f,
    123.0f,     64.0f,     -250.0f,     1.0f,
};

static void CheckInvariants(const RendererFrameSetup *setup,
    RenderProjection projection, float fov, int32_t width, int32_t height,
    const char *name)
{
    if (!PanoramaIsActive(projection, fov))
    {
        PanoramaExpect(!setup->panorama, name);
        PanoramaExpect(setup->passCount == 1u, name);
        PanoramaExpect(setup->passes[0].rectMinX == 0u, name);
        PanoramaExpect(setup->passes[0].rectMinY == 0u, name);
        PanoramaExpect(setup->passes[0].rectMaxX == (uint32_t)width, name);
        PanoramaExpect(setup->passes[0].rectMaxY == (uint32_t)height, name);
        PanoramaExpect(setup->passes[0].faceIndex == 0u, name);
        return;
    }

    PanoramaExpect(setup->panorama, name);
    PanoramaExpect(setup->passCount >= 1u, name);
    PanoramaExpect(setup->passCount <= RENDERER_MAX_SCENE_PASSES, name);
    PanoramaExpect(setup->faceResolution >= 256u, name);
    PanoramaExpect(setup->faceResolution <= 2048u, name);
    PanoramaExpect((setup->faceResolution & 255u) == 0u, name);
    RendererResolveMapping expected = projection == RENDER_PROJECTION_CYLINDER
        ? RENDERER_RESOLVE_CYLINDER : RENDERER_RESOLVE_FISHEYE;
    PanoramaExpect(setup->resolveMapping == expected, name);
    for (uint32_t slot = 0u; slot < setup->passCount; ++slot)
    {
        const RendererScenePass *pass = &setup->passes[slot];
        PanoramaExpect(pass->faceIndex < 6u, name);
        PanoramaExpect(pass->rectMinX < pass->rectMaxX, name);
        PanoramaExpect(pass->rectMinY < pass->rectMaxY, name);
        PanoramaExpect(pass->rectMaxX <= setup->faceResolution, name);
        PanoramaExpect(pass->rectMaxY <= setup->faceResolution, name);
    }
}

// Один и тот же вызов при уже прогретом кеше не должен ничего менять
// в описании кадра.
static void CheckCacheReuse(RenderProjection projection, float fov,
    int32_t width, int32_t height)
{
    PanoramaCache cache;
    ClearCache(&cache);
    RendererFrameSetup first;
    RendererFrameSetup second;
    PanoramaBuildFrameSetup(&cache, projection, fov, width, height,
        0.05f, 4096.0f, VIEW, &first);
    PanoramaBuildFrameSetup(&cache, projection, fov, width, height,
        0.05f, 4096.0f, VIEW, &second);
    PanoramaExpect(BytesEqual(&first, &second, (uint32_t)sizeof(first)),
        "cache reuse changed the frame setup");
    CheckInvariants(&first, projection, fov, width, height, "reuse");
}

// Полный пересчёт обязан воспроизводиться побайтово после сброса кеша и
// после прохода через другой набор параметров (нет неочищенного состояния).
static void CheckRebuildDeterminism(RenderProjection projection, float fov,
    int32_t width, int32_t height)
{
    PanoramaCache cache;
    ClearCache(&cache);
    RendererFrameSetup reference;
    RendererFrameSetup rebuilt;
    RendererFrameSetup afterOther;

    PanoramaBuildFrameSetup(&cache, projection, fov, width, height,
        0.05f, 4096.0f, VIEW, &reference);
    PanoramaBuildFrameSetup(&cache, RENDER_PROJECTION_FISHEYE, 200.0f,
        640, 480, 0.05f, 4096.0f, VIEW, &afterOther);
    PanoramaBuildFrameSetup(&cache, projection, fov, width, height,
        0.05f, 4096.0f, VIEW, &rebuilt);
    PanoramaExpect(BytesEqual(&reference, &rebuilt, (uint32_t)sizeof(reference)),
        "cache rebuild is not deterministic after other parameters");

    ClearCache(&cache);
    PanoramaBuildFrameSetup(&cache, projection, fov, width, height,
        0.05f, 4096.0f, VIEW, &rebuilt);
    PanoramaExpect(BytesEqual(&reference, &rebuilt, (uint32_t)sizeof(reference)),
        "cleared cache rebuilt different geometry");
}

LAIUE_TEST_ENTRY(PanoramaTestEntryPoint)
{
    PanoramaSetSceneMathService(&sceneMath);
    static const RenderProjection projections[3] = {
        RENDER_PROJECTION_AUTO, RENDER_PROJECTION_FISHEYE,
        RENDER_PROJECTION_CYLINDER,
    };
    static const float fovs[6] = {
        1.0f, 90.0f, 120.0f, 170.0f, 180.0f, 360.0f,
    };
    static const int32_t sizes[4][2] = {
        { 1, 1 }, { 64, 64 }, { 1920, 1080 }, { 3840, 2160 },
    };

    for (uint32_t p = 0u; p < 3u; ++p)
    {
        for (uint32_t f = 0u; f < 6u; ++f)
        {
            for (uint32_t s = 0u; s < 4u; ++s)
            {
                CheckCacheReuse(projections[p], fovs[f],
                    sizes[s][0], sizes[s][1]);
                CheckRebuildDeterminism(projections[p], fovs[f],
                    sizes[s][0], sizes[s][1]);
            }
        }
    }

    // Границы полей зрения поднимаются до 1 и не превосходят 360.
    {
        PanoramaCache low;
        PanoramaCache high;
        ClearCache(&low);
        ClearCache(&high);
        RendererFrameSetup lowSetup;
        RendererFrameSetup highSetup;
        PanoramaBuildFrameSetup(&low, RENDER_PROJECTION_FISHEYE, 0.0f,
            1280, 720, 0.05f, 4096.0f, VIEW, &lowSetup);
        PanoramaBuildFrameSetup(&high, RENDER_PROJECTION_FISHEYE, 1000.0f,
            1280, 720, 0.05f, 4096.0f, VIEW, &highSetup);
        RendererFrameSetup oneSetup;
        RendererFrameSetup maxSetup;
        ClearCache(&low);
        ClearCache(&high);
        PanoramaBuildFrameSetup(&low, RENDER_PROJECTION_FISHEYE, 1.0f,
            1280, 720, 0.05f, 4096.0f, VIEW, &oneSetup);
        PanoramaBuildFrameSetup(&high, RENDER_PROJECTION_FISHEYE, 360.0f,
            1280, 720, 0.05f, 4096.0f, VIEW, &maxSetup);
        PanoramaExpect(BytesEqual(&lowSetup, &oneSetup, (uint32_t)sizeof(oneSetup)),
            "fov below one must clamp to one");
        PanoramaExpect(BytesEqual(&highSetup, &maxSetup, (uint32_t)sizeof(maxSetup)),
            "fov above 360 must clamp to 360");
    }

    LaiueTestRuntimeWrite("panorama checks passed: ");
    {
        char digits[12];
        uint32_t length = 0u;
        uint32_t value = panoramaChecks;
        if (value == 0u)
        {
            digits[length++] = '0';
        }
        while (value != 0u)
        {
            digits[length++] = (char)('0' + (value % 10u));
            value /= 10u;
        }
        char text[13];
        for (uint32_t index = 0u; index < length; ++index)
        {
            text[index] = digits[length - index - 1u];
        }
        text[length] = '\0';
        LaiueTestRuntimeWrite(text);
    }
    LaiueTestRuntimeWrite("\n");
    PanoramaSetSceneMathService(NULL);
    LAIUE_TEST_SUCCESS();
}
