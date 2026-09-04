// Кадр offscreen-бэкенда целиком: устройство, пул геометрии, загрузка
// меша, проход сцены и чтение результата. Тест намеренно проверяет
// пиксели, а не только коды возврата: рендер, который «успешно» рисует
// пустой кадр, отличается от работающего только содержимым цели.

#include "content/content_catalog.h"
#include "render/chunk_geometry.h"
#include "render/renderer.h"
#include "render/renderer_offscreen.h"
#include "render/texture_pack.h"
#include "texc_fixtures.h"
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

static void PutU16(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)(value & 0xFFu);
    bytes[1] = (uint8_t)((value >> 8) & 0xFFu);
}

static void PutU32(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)(value & 0xFFu);
    bytes[1] = (uint8_t)((value >> 8) & 0xFFu);
    bytes[2] = (uint8_t)((value >> 16) & 0xFFu);
    bytes[3] = (uint8_t)((value >> 24) & 0xFFu);
}

static bool Join(wchar_t *destination, uint32_t capacity, const wchar_t *base,
                 const wchar_t *part)
{
    uint32_t length = 0u;
    while (base[length] != 0 && length + 1u < capacity)
    {
        destination[length] = base[length];
        ++length;
    }
    if (length + 1u >= capacity) return false;
    destination[length++] = L'/';

    uint32_t index = 0u;
    while (part[index] != 0 && length + 1u < capacity)
    {
        destination[length++] = part[index++];
    }
    if (part[index] != 0) return false;
    destination[length] = 0;
    return true;
}

typedef struct PackTestPaths
{
    wchar_t executable[LAIUE_PLATFORM_PATH_CAPACITY];
    wchar_t root[LAIUE_PLATFORM_PATH_CAPACITY];
    wchar_t textures[LAIUE_PLATFORM_PATH_CAPACITY];
    wchar_t pack[LAIUE_PLATFORM_PATH_CAPACITY];
    wchar_t blocks[LAIUE_PLATFORM_PATH_CAPACITY];
    wchar_t main[LAIUE_PLATFORM_PATH_CAPACITY];
    wchar_t special[LAIUE_PLATFORM_PATH_CAPACITY];
    wchar_t stone[LAIUE_PLATFORM_PATH_CAPACITY];
    wchar_t water[LAIUE_PLATFORM_PATH_CAPACITY];
    wchar_t sand[LAIUE_PLATFORM_PATH_CAPACITY];
    wchar_t glow[LAIUE_PLATFORM_PATH_CAPACITY];
    wchar_t stoneCache[LAIUE_PLATFORM_PATH_CAPACITY];
    wchar_t waterCache[LAIUE_PLATFORM_PATH_CAPACITY];
    wchar_t sandCache[LAIUE_PLATFORM_PATH_CAPACITY];
    wchar_t uneven[LAIUE_PLATFORM_PATH_CAPACITY];
    wchar_t unevenCache[LAIUE_PLATFORM_PATH_CAPACITY];
    wchar_t stoneAuthored[LAIUE_PLATFORM_PATH_CAPACITY];
    wchar_t formats[LAIUE_PLATFORM_PATH_CAPACITY];
} PackTestPaths;

static int32_t AbsoluteDifference(int32_t left, int32_t right)
{
    int32_t difference = left - right;
    return difference < 0 ? -difference : difference;
}

// Одна текстура своего формата: 4x4 одного цвета, один кадр, без карты
// нормалей. Раскладка описана в docs/texturepacks.md.
#define SINGLE_TEXTURE_BYTES (24u + 4u * 4u * 4u)

static void BuildSingleTexture(uint8_t *file, uint8_t red, uint8_t green, uint8_t blue)
{
    PutU32(file, 0x3153544Cu);   // LTS1
    PutU16(file + 4, 1u);        // версия
    PutU16(file + 6, 24u);       // размер заголовка
    PutU16(file + 8, 4u);        // ширина
    PutU16(file + 10, 4u);       // высота
    PutU16(file + 12, 1u);       // кадров
    PutU16(file + 14, 0u);       // миллисекунд на кадр
    PutU16(file + 16, 1u);       // только albedo
    PutU16(file + 18, 0u);       // зарезервировано
    PutU32(file + 20, 4u * 4u * 4u);

    for (uint32_t texel = 0; texel < 16u; ++texel)
    {
        file[24u + texel * 4u + 0u] = red;
        file[24u + texel * 4u + 1u] = green;
        file[24u + texel * 4u + 2u] = blue;
        file[24u + texel * 4u + 3u] = 255u;
    }
}

// Рисует один квад материала и отдаёт цвет центра кадра. Проверок
// приоритета форматов несколько, и каждая отличается только этим цветом.
static void SampleMaterialCentre(Renderer *renderer, RendererFrameSetup *setup,
                                 const float *origin, uint32_t blockType, double seconds,
                                 uint8_t *pixels, uint8_t outColor[3])
{
    ChunkQuad quad[1];
    quad[0] = PackChunkQuad(0u, 0u, 0u, 4u, blockType, 1u, 1u, 1u);
    RendererMesh *mesh = RendererCreateMesh(renderer, quad, 1u);
    Expect(mesh != NULL, "the sampled mesh could not be created");
    setup->animationSeconds = seconds;
    Expect(RendererBeginFrame(renderer, setup), "the sampled frame could not begin");
    RendererBeginScenePass(renderer, 0u);
    RendererDrawMesh(renderer, mesh, origin);
    Expect(RendererEndFrame(renderer), "the sampled frame could not end");

    uint32_t width = 0u;
    uint32_t height = 0u;
    Expect(RendererCaptureFrame(renderer, pixels, TEST_PIXEL_BYTES, &width, &height),
           "the sampled frame could not be captured");
    const uint8_t *centre =
        pixels + ((size_t)(TEST_HEIGHT / 2u) * TEST_WIDTH + TEST_WIDTH / 2u) * 4u;
    outColor[0] = centre[0];
    outColor[1] = centre[1];
    outColor[2] = centre[2];
    RendererDestroyMesh(renderer, mesh);
}

static bool ColorMatches(const uint8_t color[3], uint8_t red, uint8_t green, uint8_t blue)
{
    return AbsoluteDifference(color[0], red) <= 6 && AbsoluteDifference(color[1], green) <= 6 &&
           AbsoluteDifference(color[2], blue) <= 6;
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

    // === Текстурпак как папка ===
    // Пак — каталог с обычными файлами: имя материала задаёт приложение,
    // файл лежит по подпапкам, а расширение движок подбирает сам.
    // Проверяется цвет пикселя: загрузка, которая «успешно» показывает
    // не ту текстуру, отличается от работающей только им.
    RendererResize(renderer, (int32_t)TEST_WIDTH, (int32_t)TEST_HEIGHT);

    PackTestPaths *paths = PlatformAllocate(sizeof(*paths), true);
    Expect(paths != NULL, "path scratch could not be allocated");
    Expect(PlatformExecutableDirectory(paths->executable, LAIUE_PLATFORM_PATH_CAPACITY),
           "executable directory");
    Expect(Join(paths->root, LAIUE_PLATFORM_PATH_CAPACITY, paths->executable,
                L"renderer_texture_pack_test") &&
               Join(paths->textures, LAIUE_PLATFORM_PATH_CAPACITY, paths->root, L"textures") &&
               Join(paths->pack, LAIUE_PLATFORM_PATH_CAPACITY, paths->textures, L"Anim.ltp") &&
               Join(paths->blocks, LAIUE_PLATFORM_PATH_CAPACITY, paths->pack, L"blocks") &&
               Join(paths->main, LAIUE_PLATFORM_PATH_CAPACITY, paths->blocks, L"main") &&
               Join(paths->special, LAIUE_PLATFORM_PATH_CAPACITY, paths->blocks, L"special") &&
               Join(paths->stone, LAIUE_PLATFORM_PATH_CAPACITY, paths->main, L"stone.png") &&
               Join(paths->water, LAIUE_PLATFORM_PATH_CAPACITY, paths->main, L"water.gif") &&
               Join(paths->sand, LAIUE_PLATFORM_PATH_CAPACITY, paths->main, L"sand.jpg") &&
               Join(paths->glow, LAIUE_PLATFORM_PATH_CAPACITY, paths->special, L"glow.lt") &&
               Join(paths->stoneCache, LAIUE_PLATFORM_PATH_CAPACITY, paths->main,
                    L"stone.png.lt") &&
               Join(paths->waterCache, LAIUE_PLATFORM_PATH_CAPACITY, paths->main,
                    L"water.gif.lt") &&
               Join(paths->sandCache, LAIUE_PLATFORM_PATH_CAPACITY, paths->main, L"sand.jpg.lt") &&
               Join(paths->uneven, LAIUE_PLATFORM_PATH_CAPACITY, paths->main, L"uneven.gif") &&
               Join(paths->unevenCache, LAIUE_PLATFORM_PATH_CAPACITY, paths->main,
                    L"uneven.gif.lt") &&
               Join(paths->stoneAuthored, LAIUE_PLATFORM_PATH_CAPACITY, paths->main, L"stone.lt") &&
               Join(paths->formats, LAIUE_PLATFORM_PATH_CAPACITY, paths->textures, L"formats.txt"),
           "path construction");
    Expect(PlatformCreateDirectory(paths->root) && PlatformCreateDirectory(paths->textures) &&
               PlatformCreateDirectory(paths->pack) && PlatformCreateDirectory(paths->blocks) &&
               PlatformCreateDirectory(paths->main) && PlatformCreateDirectory(paths->special),
           "directory creation");

    PlatformDeleteFile(paths->stoneCache);
    PlatformDeleteFile(paths->waterCache);
    PlatformDeleteFile(paths->sandCache);
    PlatformDeleteFile(paths->unevenCache);
    // Прерванный прогон мог оставить и запасной `.lt`, и порядок
    // форматов: следующий начал бы с чужой настройкой.
    PlatformDeleteFile(paths->stoneAuthored);
    PlatformDeleteFile(paths->formats);
    Expect(PlatformWriteEntireFile(paths->stone, PNG_SOLID_FILE, sizeof(PNG_SOLID_FILE)),
           "the png layer could not be written");
    Expect(PlatformWriteEntireFile(paths->water, GIF_ANIMATED_FILE, sizeof(GIF_ANIMATED_FILE)),
           "the animated gif layer could not be written");
    Expect(PlatformWriteEntireFile(paths->sand, JPEG_SOLID_FILE, sizeof(JPEG_SOLID_FILE)),
           "the jpeg layer could not be written");
    Expect(PlatformWriteEntireFile(paths->uneven, GIF_VARIABLE_FILE, sizeof(GIF_VARIABLE_FILE)),
           "the unevenly timed gif could not be written");

    uint8_t single[SINGLE_TEXTURE_BYTES];
    BuildSingleTexture(single, 255u, 0u, 255u);
    Expect(PlatformWriteEntireFile(paths->glow, single, sizeof(single)),
           "the single texture could not be written");

    LaiueContentCatalog *catalog = LaiueContentCatalogCreate(paths->root);
    Expect(catalog != NULL, "content catalog could not be created");
    Expect(TexturePackActivateIn(catalog, L"Anim.ltp"), "the pack could not be activated");

    static const wchar_t *const materialNames[6] = {
        L"blocks/main/stone",
        L"blocks/main/water",
        L"blocks/main/sand",
        L"blocks/special/glow",
        L"blocks/main/uneven",
        L"blocks/missing",
    };
    Expect(RendererSetMaterialNames(renderer, materialNames, 6u),
           "material names with subdirectories must be accepted");

    // Выйти за пределы пака нельзя даже опечаткой в имени.
    static const wchar_t *const escapingName[1] = {L"blocks/../../secret"};
    Expect(!RendererSetMaterialNames(renderer, escapingName, 1u),
           "a name that leaves the pack must be rejected");
    Expect(RendererSetMaterialNames(renderer, materialNames, 6u),
           "the material names must survive a rejected set");

    Expect(RendererReloadTexturePackFrom(renderer, catalog), "the pack directory could not load");
    Expect(RendererGetTexturePackLoadStatus(renderer) == RENDERER_CONTENT_INCOMPLETE,
           "a pack missing one material must say so, not fail");

    // Свет без ambient и с гаммой 1: цвет текстуры доходит до пикселя
    // как есть, и ожидание считается на бумаге.
    setup.ambientColor[0] = 0.0f;
    setup.ambientColor[1] = 0.0f;
    setup.ambientColor[2] = 0.0f;
    setup.skyColor[0] = 1.0f;
    setup.skyColor[1] = 1.0f;
    setup.skyColor[2] = 0.0f;
    setup.passCount = 1u;
    setup.passes[0].rectMaxX = TEST_WIDTH;
    setup.passes[0].rectMaxY = TEST_HEIGHT;

    static const struct
    {
        uint32_t blockType;
        double seconds;
        uint8_t red;
        uint8_t green;
        uint8_t blue;
        const char *reason;
    } expectations[] = {
        {1u, 0.0, 0u, 255u, 0u, "the png layer must reach the screen"},
        {2u, 0.0, 255u, 0u, 0u, "the first gif frame must show at time zero"},
        {2u, 0.07, 0u, 255u, 0u, "the second gif frame must show after one delay"},
        {2u, 0.13, 0u, 0u, 255u, "the third gif frame must show after two delays"},
        {3u, 0.0, 0u, 255u, 255u, "the jpeg layer must reach the screen"},
        {4u, 0.0, 255u, 0u, 255u, "the prepared single texture must reach the screen"},
        // Задержки 40, 120 и 60 мс: границы кадров лежат на 40 и 160 мс,
        // цикл длится 220. Моменты выбраны так, чтобы отличить настоящее
        // расписание от двух неверных. Средний интервал (73 мс) на 50 мс
        // показал бы ещё первый кадр, а на 150 — уже третий. Прежнее
        // поведение — задержка первого кадра на всех — на 100 мс дало бы
        // третий кадр, а на 150 мс укоротило бы цикл до 120 и вернуло
        // первый.
        {5u, 0.02, 255u, 0u, 0u, "the first frame must hold for its own forty milliseconds"},
        {5u, 0.05, 0u, 255u, 0u, "the second frame must start at 40 ms, not at the average"},
        {5u, 0.10, 0u, 255u, 0u, "the long middle frame must still be showing at 100 ms"},
        {5u, 0.15, 0u, 255u, 0u, "the middle frame must hold all of its 120 ms"},
        {5u, 0.20, 0u, 0u, 255u, "the last frame must start only after 160 ms"},
        {5u, 0.23, 255u, 0u, 0u, "the cycle must be the sum of the frame delays"},
    };

    for (uint32_t index = 0; index < sizeof(expectations) / sizeof(expectations[0]); ++index)
    {
        ChunkQuad materialQuad[1];
        materialQuad[0] = PackChunkQuad(0u, 0u, 0u, 4u, expectations[index].blockType, 1u, 1u, 1u);
        RendererMesh *materialMesh = RendererCreateMesh(renderer, materialQuad, 1u);
        Expect(materialMesh != NULL, "the material mesh could not be created");

        setup.animationSeconds = expectations[index].seconds;
        Expect(RendererBeginFrame(renderer, &setup), "the material frame could not begin");
        RendererBeginScenePass(renderer, 0u);
        RendererDrawMesh(renderer, materialMesh, origin);
        Expect(RendererEndFrame(renderer), "the material frame could not end");
        Expect(RendererCaptureFrame(renderer, pixels, TEST_PIXEL_BYTES, &width, &height),
               "the material frame could not be captured");
        Expect(width == TEST_WIDTH && height == TEST_HEIGHT,
               "the material frame must be captured at full size");

        const uint8_t *centre =
            pixels + ((size_t)(TEST_HEIGHT / 2u) * TEST_WIDTH + TEST_WIDTH / 2u) * 4u;
        Expect(AbsoluteDifference(centre[0], expectations[index].red) <= 6 &&
                   AbsoluteDifference(centre[1], expectations[index].green) <= 6 &&
                   AbsoluteDifference(centre[2], expectations[index].blue) <= 6,
               expectations[index].reason);

        RendererDestroyMesh(renderer, materialMesh);
    }

    // Материал, которого в паке нет, показывает нейтральный серый.
    // Точное значение не проверяется намеренно: цель кадра в sRGB, и
    // байт на экране зависит от кривой переноса, а не от загрузчика.
    // Остальные цвета выше — крайние точки этой кривой, поэтому у них
    // сравнение точное.
    ChunkQuad missingQuad[1];
    missingQuad[0] = PackChunkQuad(0u, 0u, 0u, 4u, 6u, 1u, 1u, 1u);
    RendererMesh *missingMesh = RendererCreateMesh(renderer, missingQuad, 1u);
    Expect(missingMesh != NULL, "the mesh for the missing material could not be created");
    setup.animationSeconds = 0.0;
    Expect(RendererBeginFrame(renderer, &setup), "the missing material frame could not begin");
    RendererBeginScenePass(renderer, 0u);
    RendererDrawMesh(renderer, missingMesh, origin);
    Expect(RendererEndFrame(renderer), "the missing material frame could not end");
    Expect(RendererCaptureFrame(renderer, pixels, TEST_PIXEL_BYTES, &width, &height),
           "the missing material frame could not be captured");
    const uint8_t *neutral =
        pixels + ((size_t)(TEST_HEIGHT / 2u) * TEST_WIDTH + TEST_WIDTH / 2u) * 4u;
    Expect(AbsoluteDifference(neutral[0], neutral[1]) <= 2 &&
               AbsoluteDifference(neutral[1], neutral[2]) <= 2,
           "a material absent from the pack must show a neutral grey");
    Expect(neutral[0] > 120u && neutral[0] < 240u,
           "the neutral layer must be neither black, nor white, nor the sky");
    RendererDestroyMesh(renderer, missingMesh);

    // Цикл замыкается: пройдя все кадры, анимация возвращается к началу.
    ChunkQuad waterQuad[1];
    waterQuad[0] = PackChunkQuad(0u, 0u, 0u, 4u, 2u, 1u, 1u, 1u);
    RendererMesh *waterMesh = RendererCreateMesh(renderer, waterQuad, 1u);
    Expect(waterMesh != NULL, "the looping mesh could not be created");
    setup.animationSeconds = 0.18;
    Expect(RendererBeginFrame(renderer, &setup), "the looping frame could not begin");
    RendererBeginScenePass(renderer, 0u);
    RendererDrawMesh(renderer, waterMesh, origin);
    Expect(RendererEndFrame(renderer), "the looping frame could not end");
    Expect(RendererCaptureFrame(renderer, pixels, TEST_PIXEL_BYTES, &width, &height),
           "the looping frame could not be captured");
    const uint8_t *looped =
        pixels + ((size_t)(TEST_HEIGHT / 2u) * TEST_WIDTH + TEST_WIDTH / 2u) * 4u;
    Expect(looped[0] > 200u && looped[2] < 60u,
           "the animation must return to its first frame after one cycle");
    RendererDestroyMesh(renderer, waterMesh);

    // === Кэш рядом с исходником ===
    // Движок читает чужой формат один раз и кладёт рядом готовый `.lt`.
    // Проверяется не только его появление: исходники удаляются, пак
    // перезагружается, и те же материалы обязаны прийти из кэша.
    Expect(PlatformPathExists(paths->stoneCache) && PlatformPathExists(paths->waterCache) &&
               PlatformPathExists(paths->sandCache) && PlatformPathExists(paths->unevenCache),
           "loading a foreign format must leave a prepared texture next to it");

    // === Приоритет форматов ===
    // Свой `.lt` — запасной путь, а не главный. Пока PNG лежит рядом и
    // читается, показывается он: иначе `.lt`, собранный когда-то,
    // навсегда заслонил бы обновлённую рядом картинку, и «положи новый
    // файл» как способ поменять текстуру перестало бы работать.
    uint8_t centreColor[3];
    BuildSingleTexture(single, 255u, 0u, 0u);
    Expect(PlatformWriteEntireFile(paths->stoneAuthored, single, sizeof(single)),
           "the authored texture could not be written");
    Expect(RendererReloadTexturePackFrom(renderer, catalog),
           "the pack could not be reloaded beside an authored texture");
    SampleMaterialCentre(renderer, &setup, origin, 1u, 0.0, pixels, centreColor);
    Expect(ColorMatches(centreColor, 0u, 255u, 0u),
           "a live source must win over an authored .lt");

    // `formats.txt` переставляет порядок. Свой формат первым — берётся
    // он, и рядом ничего не появляется: выводить его не из чего.
    Expect(PlatformDeleteFile(paths->stoneCache), "the cache could not be removed");
    static const char formatsOwnFirst[] = "# priority\nlt\npng\n";
    Expect(PlatformWriteEntireFile(paths->formats, formatsOwnFirst, sizeof(formatsOwnFirst) - 1u),
           "formats.txt could not be written");
    Expect(RendererReloadTexturePackFrom(renderer, catalog),
           "the pack could not be reloaded with the engine format first");
    SampleMaterialCentre(renderer, &setup, origin, 1u, 0.0, pixels, centreColor);
    Expect(ColorMatches(centreColor, 255u, 0u, 0u),
           "formats.txt must be able to put the engine format first");
    Expect(!PlatformPathExists(paths->stoneCache),
           "nothing may be built next to a source the engine never read");

    // Формат, которого в файле нет, не исчезает — он идёт следом.
    // Иначе одна забытая строка прятала бы содержимое пака.
    static const char formatsOnlyOwn[] = "lt\n";
    Expect(PlatformWriteEntireFile(paths->formats, formatsOnlyOwn, sizeof(formatsOnlyOwn) - 1u),
           "formats.txt could not be rewritten");
    Expect(PlatformDeleteFile(paths->stoneAuthored), "the authored texture could not be removed");
    Expect(RendererReloadTexturePackFrom(renderer, catalog),
           "the pack could not be reloaded without the authored texture");
    SampleMaterialCentre(renderer, &setup, origin, 1u, 0.0, pixels, centreColor);
    Expect(ColorMatches(centreColor, 0u, 255u, 0u),
           "a format missing from formats.txt must still be reachable");
    Expect(PlatformDeleteFile(paths->formats), "formats.txt could not be removed");

    // Ради чего свой формат и стоит последним: исходник испортился —
    // показывается заранее собранный `.lt`, а не нейтральный слой.
    BuildSingleTexture(single, 255u, 0u, 0u);
    Expect(PlatformWriteEntireFile(paths->stoneAuthored, single, sizeof(single)),
           "the authored texture could not be restored");
    Expect(PlatformDeleteFile(paths->stoneCache), "the rebuilt cache could not be removed");
    static const uint8_t rubbish[64] = {0};
    Expect(PlatformWriteEntireFile(paths->stone, rubbish, sizeof(rubbish)),
           "the damaged png could not be written");
    Expect(RendererReloadTexturePackFrom(renderer, catalog),
           "a damaged source must not fail the pack");
    SampleMaterialCentre(renderer, &setup, origin, 1u, 0.0, pixels, centreColor);
    Expect(ColorMatches(centreColor, 255u, 0u, 0u),
           "a damaged source must fall through to the authored .lt");
    Expect(!PlatformPathExists(paths->stoneCache), "a damaged source must not leave a cache");

    // Дальше тест проверяет жизнь кэша, и запасной путь ему помешал бы.
    Expect(PlatformWriteEntireFile(paths->stone, PNG_SOLID_FILE, sizeof(PNG_SOLID_FILE)),
           "the png layer could not be restored");
    Expect(PlatformDeleteFile(paths->stoneAuthored), "the authored texture could not be removed");
    Expect(RendererReloadTexturePackFrom(renderer, catalog),
           "the pack could not be reloaded from the restored png");
    Expect(PlatformPathExists(paths->stoneCache), "the restored png must be cached again");

    Expect(PlatformDeleteFile(paths->stone) && PlatformDeleteFile(paths->water) &&
               PlatformDeleteFile(paths->sand) && PlatformDeleteFile(paths->uneven),
           "the source images could not be removed");
    Expect(RendererReloadTexturePackFrom(renderer, catalog),
           "the pack must load from the cache alone");

    static const struct
    {
        uint32_t blockType;
        double seconds;
        uint8_t red;
        uint8_t green;
        uint8_t blue;
        const char *reason;
    } cached[] = {
        {1u, 0.0, 0u, 255u, 0u, "the png material must survive its source"},
        {2u, 0.07, 0u, 255u, 0u, "the animated material must survive its source"},
        {3u, 0.0, 0u, 255u, 255u, "the jpeg material must survive its source"},
        // Неровное расписание обязано пережить перекладку в `.lt`:
        // на 100 мс всё ещё второй кадр, а не третий.
        {5u, 0.05, 0u, 255u, 0u, "the uneven schedule must survive the cache"},
        {5u, 0.15, 0u, 255u, 0u, "the long frame must still hold all of its 120 ms"},
        {5u, 0.20, 0u, 0u, 255u, "the last frame must still start only after 160 ms"},
    };
    for (uint32_t index = 0; index < sizeof(cached) / sizeof(cached[0]); ++index)
    {
        ChunkQuad quad[1];
        quad[0] = PackChunkQuad(0u, 0u, 0u, 4u, cached[index].blockType, 1u, 1u, 1u);
        RendererMesh *mesh = RendererCreateMesh(renderer, quad, 1u);
        Expect(mesh != NULL, "the cached mesh could not be created");
        setup.animationSeconds = cached[index].seconds;
        Expect(RendererBeginFrame(renderer, &setup), "the cached frame could not begin");
        RendererBeginScenePass(renderer, 0u);
        RendererDrawMesh(renderer, mesh, origin);
        Expect(RendererEndFrame(renderer), "the cached frame could not end");
        Expect(RendererCaptureFrame(renderer, pixels, TEST_PIXEL_BYTES, &width, &height),
               "the cached frame could not be captured");
        const uint8_t *centre =
            pixels + ((size_t)(TEST_HEIGHT / 2u) * TEST_WIDTH + TEST_WIDTH / 2u) * 4u;
        Expect(AbsoluteDifference(centre[0], cached[index].red) <= 6 &&
                   AbsoluteDifference(centre[1], cached[index].green) <= 6 &&
                   AbsoluteDifference(centre[2], cached[index].blue) <= 6,
               cached[index].reason);
        RendererDestroyMesh(renderer, mesh);
    }

    // Нет ни исходника, ни кэша — остаётся нейтральный слой. Это и есть
    // «текстура по умолчанию для всего»: пак грузится, а не отказывает.
    Expect(PlatformDeleteFile(paths->stoneCache) && PlatformDeleteFile(paths->waterCache) &&
               PlatformDeleteFile(paths->sandCache) && PlatformDeleteFile(paths->unevenCache),
           "the cached textures could not be removed");
    Expect(RendererReloadTexturePackFrom(renderer, catalog),
           "the pack must still load with nothing but the prepared texture");
    Expect(RendererGetTexturePackLoadStatus(renderer) == RENDERER_CONTENT_INCOMPLETE,
           "a pack that lost its sources must say so, not fail");

    ChunkQuad goneQuad[1];
    goneQuad[0] = PackChunkQuad(0u, 0u, 0u, 4u, 1u, 1u, 1u, 1u);
    RendererMesh *goneMesh = RendererCreateMesh(renderer, goneQuad, 1u);
    Expect(goneMesh != NULL, "the mesh for the vanished material could not be created");
    setup.animationSeconds = 0.0;
    Expect(RendererBeginFrame(renderer, &setup), "the vanished material frame could not begin");
    RendererBeginScenePass(renderer, 0u);
    RendererDrawMesh(renderer, goneMesh, origin);
    Expect(RendererEndFrame(renderer), "the vanished material frame could not end");
    Expect(RendererCaptureFrame(renderer, pixels, TEST_PIXEL_BYTES, &width, &height),
           "the vanished material frame could not be captured");
    const uint8_t *vanished =
        pixels + ((size_t)(TEST_HEIGHT / 2u) * TEST_WIDTH + TEST_WIDTH / 2u) * 4u;
    Expect(AbsoluteDifference(vanished[0], vanished[1]) <= 2 &&
               AbsoluteDifference(vanished[1], vanished[2]) <= 2 && vanished[0] > 120u &&
               vanished[0] < 240u,
           "a material without a source and without a cache must show the neutral layer");
    RendererDestroyMesh(renderer, goneMesh);

    // Повреждённый файл теряет свой материал, но не весь пак: сосед по
    // папке не должен пропадать из-за чужой опечатки.
    BuildSingleTexture(single, 0u, 255u, 255u);
    PutU16(single + 12, 0u);   // кадров ноль
    Expect(PlatformWriteEntireFile(paths->glow, single, sizeof(single)),
           "the damaged texture could not be written");
    Expect(RendererReloadTexturePackFrom(renderer, catalog),
           "one damaged file must not fail the whole pack");
    Expect(RendererGetTexturePackLoadStatus(renderer) == RENDERER_CONTENT_INCOMPLETE,
           "a damaged file must count as a missing material");

    LaiueContentCatalogDestroy(catalog);
    PlatformDeleteFile(paths->stone);
    PlatformDeleteFile(paths->water);
    PlatformDeleteFile(paths->glow);
    PlatformFree(paths);

    RendererDestroyMesh(renderer, mesh);
    PlatformFree(pixels);
    RendererDestroy(renderer);

    LaiueTestRuntimeWrite("Offscreen renderer frame checks passed\n");
    LAIUE_TEST_SUCCESS();
}
