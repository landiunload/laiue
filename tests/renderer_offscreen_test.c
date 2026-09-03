// Кадр offscreen-бэкенда целиком: устройство, пул геометрии, загрузка
// меша, проход сцены и чтение результата. Тест намеренно проверяет
// пиксели, а не только коды возврата: рендер, который «успешно» рисует
// пустой кадр, отличается от работающего только содержимым цели.

#include "render/chunk_geometry.h"
#include "render/renderer.h"
#include "render/renderer_offscreen.h"
#include "platform/system.h"
#include "test_runtime.h"

#include <stdbool.h>
#include <stdint.h>

#define TEST_WIDTH 64u
#define TEST_HEIGHT 64u
#define TEST_PIXEL_BYTES (TEST_WIDTH * TEST_HEIGHT * 4u)

static void Expect(bool condition, const char *message)
{
    if (condition) return;
    LaiueTestRuntimeWrite("Offscreen renderer check failed: ");
    LaiueTestRuntimeWrite(message);
    LaiueTestRuntimeWrite("\n");
    LaiueTestRuntimeExit(1);
}

// Единичная матрица: мировые координаты попадают прямо в clip space,
// поэтому ожидаемое положение квада считается на бумаге и не зависит
// ни от камеры, ни от проекции.
static void SetIdentity(float matrix[16])
{
    for (uint32_t index = 0; index < 16u; ++index) matrix[index] = 0.0f;
    matrix[0] = 1.0f;
    matrix[5] = 1.0f;
    matrix[10] = 1.0f;
    matrix[15] = 1.0f;
}

static uint32_t CountPixelsDifferentFrom(const uint8_t *pixels, uint8_t red, uint8_t green,
                                         uint8_t blue, uint32_t tolerance)
{
    uint32_t different = 0u;
    for (uint32_t index = 0; index < TEST_WIDTH * TEST_HEIGHT; ++index)
    {
        const uint8_t *pixel = pixels + (size_t)index * 4u;
        uint32_t deltaRed = pixel[0] > red ? (uint32_t)(pixel[0] - red) : (uint32_t)(red - pixel[0]);
        uint32_t deltaGreen =
            pixel[1] > green ? (uint32_t)(pixel[1] - green) : (uint32_t)(green - pixel[1]);
        uint32_t deltaBlue =
            pixel[2] > blue ? (uint32_t)(pixel[2] - blue) : (uint32_t)(blue - pixel[2]);
        if (deltaRed > tolerance || deltaGreen > tolerance || deltaBlue > tolerance) ++different;
    }
    return different;
}

LAIUE_TEST_ENTRY(RendererOffscreenTestEntryPoint)
{
    Renderer *renderer = RendererCreate(NULL, (int32_t)TEST_WIDTH, (int32_t)TEST_HEIGHT);
    Expect(renderer != NULL, "offscreen renderer could not be created");
    Expect(!RendererIsWorldReady(renderer), "a fresh renderer must not report a ready world");

    // Активного текстурпака нет: рендерер обязан подняться на встроенных
    // заглушках, а не отказать.
    Expect(RendererPrepareWorld(renderer), "world resources could not be prepared");
    Expect(RendererIsWorldReady(renderer), "world must be ready after preparation");

    uint8_t *pixels = PlatformAllocate(TEST_PIXEL_BYTES, true);
    Expect(pixels != NULL, "readback buffer could not be allocated");

    // === Кадр без проходов сцены: только очистка ===
    RendererFrameSetup setup;
    for (uint32_t index = 0; index < sizeof(setup); ++index) ((uint8_t *)&setup)[index] = 0u;
    setup.gamma = 1.0f;
    setup.skyColor[0] = 1.0f;
    setup.skyColor[1] = 0.0f;
    setup.skyColor[2] = 0.0f;
    setup.passCount = 0u;

    Expect(RendererBeginFrame(renderer, &setup), "empty frame could not begin");
    Expect(RendererEndFrame(renderer), "empty frame could not end");

    uint32_t width = 0u;
    uint32_t height = 0u;
    Expect(RendererCaptureFrame(renderer, pixels, TEST_PIXEL_BYTES, &width, &height),
           "empty frame could not be captured");
    Expect(width == TEST_WIDTH && height == TEST_HEIGHT, "captured frame has the wrong size");
    Expect(CountPixelsDifferentFrom(pixels, 255u, 0u, 0u, 2u) == 0u,
           "a frame without scene passes must be filled with the sky colour");

    // === Кадр с геометрией ===
    // Одна грань +Z единичного вокселя. С единичной матрицей и смещением
    // -0.5 по X и Y квад накрывает ровно центральную половину экрана.
    ChunkQuad quads[1];
    quads[0] = PackChunkQuad(0u, 0u, 0u, 4u, 1u, 1u, 1u, 1u);

    RendererMesh *mesh = RendererCreateMesh(renderer, quads, 1u);
    Expect(mesh != NULL, "mesh could not be created");

    setup.passCount = 1u;
    setup.skyColor[0] = 1.0f;
    setup.skyColor[1] = 0.0f;
    setup.skyColor[2] = 0.0f;
    setup.sunDirection[0] = 0.0f;
    setup.sunDirection[1] = 0.0f;
    setup.sunDirection[2] = -1.0f;
    setup.sunColor[0] = 1.0f;
    setup.sunColor[1] = 1.0f;
    setup.sunColor[2] = 1.0f;
    setup.ambientColor[0] = 1.0f;
    setup.ambientColor[1] = 1.0f;
    setup.ambientColor[2] = 1.0f;
    SetIdentity(setup.passes[0].viewProjection);
    setup.passes[0].faceIndex = 0u;
    setup.passes[0].rectMinX = 0u;
    setup.passes[0].rectMinY = 0u;
    setup.passes[0].rectMaxX = TEST_WIDTH;
    setup.passes[0].rectMaxY = TEST_HEIGHT;

    const float origin[3] = { -0.5f, -0.5f, -0.5f };
    Expect(RendererBeginFrame(renderer, &setup), "scene frame could not begin");
    RendererBeginScenePass(renderer, 0u);
    RendererDrawMesh(renderer, mesh, origin);
    Expect(RendererEndFrame(renderer), "scene frame could not end");

    RendererStats stats;
    RendererGetStats(renderer, &stats);
    Expect(stats.drawCalls == 1u, "the scene pass must record exactly one draw call");
    Expect(stats.drawnQuads == 1u, "the scene pass must record exactly one quad");
    Expect(stats.scenePasses == 1u, "the frame must report one scene pass");
    Expect(stats.uploadedBytes == sizeof(quads), "the mesh upload must be recorded once");
    Expect(stats.geometryPoolCapacityBytes > 0u, "the geometry pool must report its capacity");
    Expect(stats.geometryPoolUsedBytes >= sizeof(quads),
           "the geometry pool must account for the mesh");

    Expect(RendererCaptureFrame(renderer, pixels, TEST_PIXEL_BYTES, &width, &height),
           "scene frame could not be captured");

    // Квад занимает центральную половину кадра: четверть площади.
    uint32_t covered = CountPixelsDifferentFrom(pixels, 255u, 0u, 0u, 8u);
    Expect(covered > (TEST_WIDTH * TEST_HEIGHT) / 8u,
           "the drawn quad covers far less of the frame than its geometry implies");
    Expect(covered < (TEST_WIDTH * TEST_HEIGHT * 3u) / 8u,
           "the drawn quad covers far more of the frame than its geometry implies");

    // Центр кадра принадлежит квадру, а углы — небу. Это отличает
    // настоящую отрисовку от случайной заливки.
    const uint8_t *center = pixels + ((TEST_HEIGHT / 2u) * TEST_WIDTH + TEST_WIDTH / 2u) * 4u;
    Expect(!(center[0] > 250u && center[1] < 5u && center[2] < 5u),
           "the centre of the frame must show the quad, not the sky");
    const uint8_t *corner = pixels;
    Expect(corner[0] > 250u && corner[1] < 5u && corner[2] < 5u,
           "the frame corner must stay the sky colour");

    // === Инстансный путь ===
    // Тот же меш рисуется дважды со своими смещениями: в chunk.hlsl это
    // отдельная ветка, которую обычная отрисовка не задевает.
    RendererMeshInstance instances[2];
    instances[0].originRelative[0] = -0.5f;
    instances[0].originRelative[1] = -0.5f;
    instances[0].originRelative[2] = -0.5f;
    instances[0].scale = 0.5f;
    instances[1].originRelative[0] = 0.0f;
    instances[1].originRelative[1] = 0.0f;
    instances[1].originRelative[2] = -0.5f;
    instances[1].scale = 0.5f;

    Expect(RendererBeginFrame(renderer, &setup), "instanced frame could not begin");
    RendererBeginScenePass(renderer, 0u);
    RendererDrawMeshInstances(renderer, mesh, instances, 2u);
    Expect(RendererEndFrame(renderer), "instanced frame could not end");
    RendererGetStats(renderer, &stats);
    Expect(stats.drawCalls == 1u, "instanced drawing must issue a single draw call");
    Expect(stats.drawnQuads == 2u, "instanced drawing must count one quad per instance");
    Expect(RendererCaptureFrame(renderer, pixels, TEST_PIXEL_BYTES, &width, &height),
           "instanced frame could not be captured");
    uint32_t instanced = CountPixelsDifferentFrom(pixels, 255u, 0u, 0u, 8u);
    Expect(instanced > 0u, "instanced drawing produced an empty frame");
    Expect(instanced < covered, "half-scale instances must cover less than the full-size quad");

    // === Панорама ===
    // Шесть граней кубмапы и полноэкранный резолв. Проход рисует только
    // свой прямоугольник грани, поэтому очистка ограничена им же.
    RendererFrameSetup panorama = setup;
    panorama.panorama = true;
    panorama.faceResolution = 32u;
    panorama.resolveMapping = RENDERER_RESOLVE_FISHEYE;
    panorama.fovHalfRadians = 1.5f;
    panorama.resolveVerticalScale = 1.0f;
    panorama.passCount = RENDERER_MAX_SCENE_PASSES;
    for (uint32_t face = 0; face < RENDERER_MAX_SCENE_PASSES; ++face)
    {
        panorama.passes[face] = setup.passes[0];
        panorama.passes[face].faceIndex = face;
        panorama.passes[face].rectMaxX = panorama.faceResolution;
        panorama.passes[face].rectMaxY = panorama.faceResolution;
    }

    Expect(RendererBeginFrame(renderer, &panorama), "panorama frame could not begin");
    for (uint32_t face = 0; face < RENDERER_MAX_SCENE_PASSES; ++face)
    {
        RendererBeginScenePass(renderer, face);
        RendererDrawMesh(renderer, mesh, origin);
    }
    Expect(RendererEndFrame(renderer), "panorama frame could not end");
    RendererGetStats(renderer, &stats);
    Expect(stats.scenePasses == RENDERER_MAX_SCENE_PASSES,
           "the panorama frame must report six scene passes");
    // Шесть проходов плюс полноэкранный резолв.
    Expect(stats.drawCalls == RENDERER_MAX_SCENE_PASSES + 1u,
           "the panorama frame must resolve the cube map exactly once");
    Expect(RendererCaptureFrame(renderer, pixels, TEST_PIXEL_BYTES, &width, &height),
           "panorama frame could not be captured");
    Expect(CountPixelsDifferentFrom(pixels, 0u, 0u, 0u, 4u) > 0u,
           "the panorama resolve produced a black frame");

    // === Слой интерфейса ===
    // Без атласа шрифта слой не рисуется вовсе: это осознанное поведение
    // контракта, а не молчаливая потеря квадов.
    RendererUiQuad quad;
    for (uint32_t index = 0; index < sizeof(quad); ++index) ((uint8_t *)&quad)[index] = 0u;
    quad.rect[0] = 0.0f;
    quad.rect[1] = 0.0f;
    quad.rect[2] = (float)TEST_WIDTH;
    quad.rect[3] = (float)TEST_HEIGHT;
    quad.colorRGBA = 0xff00ff00u;   // непрозрачный зелёный

    Expect(RendererBeginFrame(renderer, &setup), "ui frame without an atlas could not begin");
    RendererBeginScenePass(renderer, 0u);
    RendererDrawMesh(renderer, mesh, origin);
    RendererUiQueue(renderer, &quad, 1u);
    Expect(RendererEndFrame(renderer), "ui frame without an atlas could not end");
    RendererGetStats(renderer, &stats);
    Expect(stats.drawCalls == 1u, "without a font atlas the ui layer must not draw");

    static const uint8_t atlas[4] = { 255u, 255u, 255u, 255u };
    Expect(RendererUiSetFontAtlas(renderer, atlas, 2u, 2u), "font atlas could not be uploaded");

    Expect(RendererBeginFrame(renderer, &setup), "ui frame could not begin");
    RendererBeginScenePass(renderer, 0u);
    RendererDrawMesh(renderer, mesh, origin);
    RendererUiQueue(renderer, &quad, 1u);
    Expect(RendererEndFrame(renderer), "ui frame could not end");
    RendererGetStats(renderer, &stats);
    Expect(stats.drawCalls == 2u, "the ui layer must add one draw call to the frame");
    Expect(RendererCaptureFrame(renderer, pixels, TEST_PIXEL_BYTES, &width, &height),
           "ui frame could not be captured");
    // Непрозрачный квад во весь экран закрывает и небо, и геометрию.
    const uint8_t *uiCorner = pixels;
    Expect(uiCorner[1] > uiCorner[0] && uiCorner[1] > uiCorner[2],
           "the opaque ui quad must cover the frame with its own colour");

    // === Изменение размера ===
    RendererResize(renderer, (int32_t)TEST_WIDTH / 2, (int32_t)TEST_HEIGHT / 2);
    setup.passCount = 0u;
    Expect(RendererBeginFrame(renderer, &setup), "frame after resize could not begin");
    Expect(RendererEndFrame(renderer), "frame after resize could not end");
    Expect(RendererCaptureFrame(renderer, pixels, TEST_PIXEL_BYTES, &width, &height),
           "frame after resize could not be captured");
    Expect(width == TEST_WIDTH / 2u && height == TEST_HEIGHT / 2u,
           "the captured frame must follow the requested size");

    // === Формат байткода шейдеров ===
    // Пак, собранный для D3D12, содержит DXBC. Бэкенд обязан отказать
    // понятно, а не отдать драйверу чужой байткод.
    static const uint8_t dxbcHeader[32] = { 'D', 'X', 'B', 'C' };
    LaiueShaderSet shaderSet;
    LaiueShaderSetInitialize(&shaderSet);
    Expect(LaiueShaderSetSetOverride(&shaderSet, LAIUE_SHADER_UI_VERTEX, dxbcHeader,
                                     (uint32_t)sizeof(dxbcHeader)),
           "the override could not be recorded");
    Expect(!RendererReloadShaderSet(renderer, &shaderSet),
           "a Vulkan backend must reject D3D bytecode");
    Expect(RendererReloadShaderSet(renderer, NULL),
           "the embedded fallback set must stay loadable after a rejected override");

    RendererDestroyMesh(renderer, mesh);
    PlatformFree(pixels);
    RendererDestroy(renderer);

    LaiueTestRuntimeWrite("Offscreen renderer frame checks passed\n");
    LAIUE_TEST_SUCCESS();
}
