// Прямая регрессия двухпроходной сборки текстурного пака
// (`TexturePackBuildFrom`): заголовочный проход, расписание кадров, карта
// нормалей, отсутствующие материалы, кэш и порядок форматов.
//
// Сборщик не экспортируется из `laiue_render`, поэтому тест компилирует
// внутренний `src/render/texture_build.c` прямо в свой исполняемый файл:
// новые экспорты не нужны, GPU не нужен, и проверка идёт на любой машине.
// `texture_pack.c` не компилируется: сверяются данные собранного пака
// (пиксели, `animation[]`, `sliceMilliseconds[]`), а не решение кадра.
//
// Круг по цвету пикселя на экране замыкает `laiue.render.offscreen_frame`;
// здесь проверяется то, что публичный путь не показывает: соответствие
// метаданных и пикселей холодной и тёплой сборки, в том числе когда
// исходников рядом уже нет.

#include "content/content_catalog.h"
#include "media/lt_encode.h"
#include "platform/system.h"
#include "render/texture_pack_internal.h"
#include "test_runtime.h"
#include "texc_fixtures.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define TEST_ROOT_NAME L"texture_pack_build_test_v1"
#define TEST_PACK_NAME L"Build.ltp"
#define PATH_CAP LAIUE_PLATFORM_PATH_CAPACITY
// Один кадр 4x4 RGBA: размеры выбраны так, чтобы `ImageResample` не менял
// ни одного отсчёта и ожидаемый цвет сверялся точно.
#define SOLID_BYTES (4u * 4u * 4u)

#if defined(_MSC_VER) || defined(__clang__)
#define TEXTURE_TEST_NOINLINE __declspec(noinline)
#else
#define TEXTURE_TEST_NOINLINE __attribute__((noinline))
#endif

static const uint8_t kRed[4] = {255u, 0u, 0u, 255u};
static const uint8_t kGreen[4] = {0u, 255u, 0u, 255u};
static const uint8_t kBlue[4] = {0u, 0u, 255u, 255u};
static const uint8_t kMagenta[4] = {255u, 0u, 255u, 255u};
static const uint8_t kMissing[4] = {160u, 160u, 160u, 255u};

static void Expect(bool condition, const char *message)
{
    if (condition)
    {
        return;
    }
    LaiueTestRuntimeWrite("texture pack build test failed: ");
    LaiueTestRuntimeWrite(message);
    LaiueTestRuntimeWrite("\n");
    LaiueTestRuntimeExit(1);
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

static bool Join(wchar_t *destination, uint32_t capacity, const wchar_t *base, const wchar_t *part)
{
    uint32_t length = 0u;
    while (base[length] != 0)
    {
        if (length + 1u >= capacity)
        {
            return false;
        }
        destination[length] = base[length];
        ++length;
    }
    if (length + 1u >= capacity)
    {
        return false;
    }
    destination[length++] = L'/';
    for (uint32_t index = 0u; part[index] != 0; ++index)
    {
        if (length + 1u >= capacity)
        {
            return false;
        }
        destination[length++] = part[index];
    }
    destination[length] = 0;
    return true;
}

static bool Append(wchar_t *destination, uint32_t capacity, const wchar_t *suffix)
{
    uint32_t length = 0u;
    while (destination[length] != 0)
    {
        ++length;
    }
    for (uint32_t index = 0u; suffix[index] != 0; ++index)
    {
        if (length + 1u >= capacity)
        {
            return false;
        }
        destination[length++] = suffix[index];
    }
    destination[length] = 0;
    return true;
}

// Пути живут в куче, а не на стеке: no-CRT сборка не линкует __chkstk, а
// несколько буферов по 32 КиБ перевалили бы предел кадра в 4 КиБ.
#define PATH_SLOTS 8u

typedef struct TestPaths
{
    wchar_t root[PATH_CAP];
    wchar_t textures[PATH_CAP];
    wchar_t pack[PATH_CAP];
    wchar_t main[PATH_CAP];
    wchar_t formats[PATH_CAP];
    wchar_t slots[PATH_SLOTS][PATH_CAP];
} TestPaths;

// Путь материала внутри пака: `<pack>/main/<name><extension>`.
static void MainFile(TestPaths *paths, wchar_t *destination, uint32_t capacity, const wchar_t *name,
                     const wchar_t *extension)
{
    Expect(Join(destination, capacity, paths->main, name) &&
               Append(destination, capacity, extension),
           "material path construction");
}

static void FillSolid(uint8_t *pixels, uint32_t width, uint32_t height, const uint8_t texel[4])
{
    uint32_t count = width * height;
    for (uint32_t index = 0u; index < count; ++index)
    {
        pixels[index * 4u + 0u] = texel[0];
        pixels[index * 4u + 1u] = texel[1];
        pixels[index * 4u + 2u] = texel[2];
        pixels[index * 4u + 3u] = texel[3];
    }
}

// Свой формат версии 1: один интервал на всю анимацию и заголовок без
// отпечатка исходника. Кодировщик `LtEncode` пишет уже версию 2, поэтому
// файл собирается вручную ровно по прежней раскладке.
static void WriteLtV1(const wchar_t *path, const uint8_t *albedo, const uint8_t *normal,
                      uint32_t width, uint32_t height, uint32_t frameCount, uint16_t duration)
{
    uint32_t frameBytes = width * height * 4u;
    uint32_t payload = frameCount * frameBytes * (normal != NULL ? 2u : 1u);
    uint32_t total = LT_HEADER_BYTES_V1 + payload;
    uint8_t *file = PlatformAllocate(total, false);
    Expect(file != NULL, "v1 scratch allocation");
    PutU32(file + 0u, 0x3153544Cu);
    PutU16(file + 4u, 1u);
    PutU16(file + 6u, LT_HEADER_BYTES_V1);
    PutU16(file + 8u, width);
    PutU16(file + 10u, height);
    PutU16(file + 12u, frameCount);
    PutU16(file + 14u, duration);
    PutU16(file + 16u, normal != NULL ? 2u : 1u);
    PutU16(file + 18u, 0u);
    PutU32(file + 20u, payload);
    memcpy(file + LT_HEADER_BYTES_V1, albedo, (size_t)frameCount * frameBytes);
    if (normal != NULL)
    {
        memcpy(file + LT_HEADER_BYTES_V1 + (size_t)frameCount * frameBytes, normal,
               (size_t)frameCount * frameBytes);
    }
    Expect(PlatformWriteEntireFile(path, file, total), "v1 texture write");
    PlatformFree(file);
}

static void WriteLtV2(const wchar_t *path, const uint8_t *albedo, const uint8_t *normal,
                      uint32_t width, uint32_t height, uint32_t frameCount,
                      const uint16_t *durations, uint64_t modifiedTime, uint32_t sourceSize)
{
    uint32_t bytes = 0u;
    Expect(LtEncodedBytes(width, height, frameCount, normal != NULL, &bytes) == LT_OK,
           "v2 encoded size");
    uint8_t *file = PlatformAllocate(bytes, false);
    Expect(file != NULL, "v2 scratch allocation");
    LtTexture texture = {
        .albedoFrames = albedo,
        .normalFrames = normal,
        .width = width,
        .height = height,
        .frameCount = frameCount,
        .frameMilliseconds = durations,
        .sourceModifiedTime = modifiedTime,
        .sourceSizeBytes = sourceSize,
    };
    Expect(LtEncode(&texture, file, bytes, NULL) == LT_OK, "v2 encode");
    Expect(PlatformWriteEntireFile(path, file, bytes), "v2 texture write");
    PlatformFree(file);
}

static uint64_t ChainBytes(uint32_t size)
{
    uint64_t total = 0u;
    while (size > 1u)
    {
        total += (uint64_t)size * size * 4u;
        size >>= 1;
    }
    total += 4u;
    return total;
}

static const uint8_t *AlbedoMip0(const TexturePackData *pack, uint32_t slice)
{
    return pack->pixels + (size_t)(ChainBytes(pack->width) * slice);
}

static const uint8_t *NormalMip0(const TexturePackData *pack, uint32_t slice)
{
    return pack->normalPixels + (size_t)(ChainBytes(pack->width) * slice);
}

static bool TexelEquals(const uint8_t *texel, const uint8_t expected[4])
{
    return texel[0] == expected[0] && texel[1] == expected[1] && texel[2] == expected[2] &&
           texel[3] == expected[3];
}

static TexturePackLoadStatus Build(LaiueContentCatalog *catalog, const wchar_t *const *names,
                                   uint32_t count, TexturePackData *outPack)
{
    memset(outPack, 0, sizeof(*outPack));
    TexturePackLoadStatus status = TexturePackBuildFrom(catalog, names, count, outPack);
    if (status == TEXTURE_PACK_LOAD_OK || status == TEXTURE_PACK_LOAD_INCOMPLETE)
    {
        Expect(outPack->pixels != NULL, "a loaded pack must carry pixels");
    }
    return status;
}

static void ReleasePack(TexturePackData *pack)
{
    PlatformFree(pack->allocation);
    memset(pack, 0, sizeof(*pack));
}

// Полная копия результата сборки: пиксели альбедо, карта нормалей и
// расписание. По ней холодная и тёплая сборка сравниваются побайтово.
typedef struct PackSnapshot
{
    uint16_t width;
    uint16_t height;
    uint16_t sliceCount;
    uint16_t mipCount;
    uint16_t materialCount;
    uint32_t pixelBytes;
    uint8_t *pixels;
    uint8_t *normals;
    TexturePackAnimation animation[TEXTURE_PACK_MAX_LAYERS];
    uint16_t sliceMilliseconds[TEXTURE_PACK_MAX_SLICES];
} PackSnapshot;

static void SnapshotCapture(const TexturePackData *pack, PackSnapshot *outSnapshot)
{
    memset(outSnapshot, 0, sizeof(*outSnapshot));
    outSnapshot->width = pack->width;
    outSnapshot->height = pack->height;
    outSnapshot->sliceCount = pack->sliceCount;
    outSnapshot->mipCount = pack->mipCount;
    outSnapshot->materialCount = pack->materialCount;
    outSnapshot->pixelBytes = pack->pixelBytes;
    outSnapshot->pixels = PlatformAllocate(pack->pixelBytes, false);
    Expect(outSnapshot->pixels != NULL, "snapshot pixel allocation");
    memcpy(outSnapshot->pixels, pack->pixels, pack->pixelBytes);
    if (pack->normalPixels != NULL)
    {
        outSnapshot->normals = PlatformAllocate(pack->pixelBytes, false);
        Expect(outSnapshot->normals != NULL, "snapshot normal allocation");
        memcpy(outSnapshot->normals, pack->normalPixels, pack->pixelBytes);
    }
    memcpy(outSnapshot->animation, pack->animation, sizeof(outSnapshot->animation));
    memcpy(outSnapshot->sliceMilliseconds, pack->sliceMilliseconds,
           sizeof(outSnapshot->sliceMilliseconds));
}

static void SnapshotRelease(PackSnapshot *snapshot)
{
    PlatformFree(snapshot->pixels);
    PlatformFree(snapshot->normals);
    memset(snapshot, 0, sizeof(*snapshot));
}

static bool SnapshotEquals(const PackSnapshot *left, const PackSnapshot *right)
{
    if (left->width != right->width || left->height != right->height ||
        left->sliceCount != right->sliceCount || left->mipCount != right->mipCount ||
        left->materialCount != right->materialCount || left->pixelBytes != right->pixelBytes)
    {
        return false;
    }
    if (memcmp(left->pixels, right->pixels, left->pixelBytes) != 0)
    {
        return false;
    }
    if ((left->normals == NULL) != (right->normals == NULL))
    {
        return false;
    }
    if (left->normals != NULL && memcmp(left->normals, right->normals, left->pixelBytes) != 0)
    {
        return false;
    }
    return memcmp(left->animation, right->animation, sizeof(left->animation)) == 0 &&
           memcmp(left->sliceMilliseconds, right->sliceMilliseconds,
                  sizeof(left->sliceMilliseconds)) == 0;
}

// === Свой формат ===

static TEXTURE_TEST_NOINLINE void TestSingleTextureV1(LaiueContentCatalog *catalog,
                                                      TestPaths *paths)
{
    uint8_t albedo[2u * SOLID_BYTES];
    FillSolid(albedo + 0u * SOLID_BYTES, 4u, 4u, kRed);
    FillSolid(albedo + 1u * SOLID_BYTES, 4u, 4u, kBlue);
    wchar_t *path = paths->slots[0];
    MainFile(paths, path, PATH_CAP, L"v1", L".lt");
    WriteLtV1(path, albedo, NULL, 4u, 4u, 2u, 100u);

    static const wchar_t *const names[1] = {L"main/v1"};
    TexturePackData pack;
    Expect(Build(catalog, names, 1u, &pack) == TEXTURE_PACK_LOAD_OK, "v1 pack status");
    Expect((uint32_t)pack.width == 4u && (uint32_t)pack.height == 4u &&
               (uint32_t)pack.mipCount == 3u && (uint32_t)pack.sliceCount == 2u &&
               (uint32_t)pack.materialCount == 1u,
           "v1 geometry");
    Expect((uint32_t)pack.animation[0].firstSlice == 0u &&
               (uint32_t)pack.animation[0].frameCount == 2u &&
               pack.animation[0].cycleMilliseconds == 200u,
           "v1 animation must follow the single duration");
    Expect((uint32_t)pack.sliceMilliseconds[0] == 100u &&
               (uint32_t)pack.sliceMilliseconds[1] == 100u,
           "v1 single duration must apply to every frame");
    Expect(TexelEquals(AlbedoMip0(&pack, 0u), kRed), "v1 first frame");
    Expect(TexelEquals(AlbedoMip0(&pack, 1u), kBlue), "v1 second frame");
    ReleasePack(&pack);
}

static TEXTURE_TEST_NOINLINE void TestSingleTextureV1Normals(LaiueContentCatalog *catalog,
                                                             TestPaths *paths)
{
    uint8_t albedo[SOLID_BYTES];
    uint8_t normal[SOLID_BYTES];
    FillSolid(albedo, 4u, 4u, kRed);
    FillSolid(normal, 4u, 4u, kMagenta);
    wchar_t *path = paths->slots[0];
    MainFile(paths, path, PATH_CAP, L"v1normal", L".lt");
    WriteLtV1(path, albedo, normal, 4u, 4u, 1u, 0u);

    static const wchar_t *const names[1] = {L"main/v1normal"};
    TexturePackData pack;
    Expect(Build(catalog, names, 1u, &pack) == TEXTURE_PACK_LOAD_OK, "v1 normal pack status");
    Expect(pack.normalPixels != NULL, "v1 format 2 must carry the normal layer");
    Expect(TexelEquals(AlbedoMip0(&pack, 0u), kRed), "v1 normal albedo");
    Expect(TexelEquals(NormalMip0(&pack, 0u), kMagenta), "v1 normal layer pixels");
    ReleasePack(&pack);
}

static TEXTURE_TEST_NOINLINE void TestSingleTextureV2Durations(LaiueContentCatalog *catalog,
                                                               TestPaths *paths)
{
    uint8_t frames[3u * SOLID_BYTES];
    FillSolid(frames + 0u * SOLID_BYTES, 4u, 4u, kRed);
    FillSolid(frames + 1u * SOLID_BYTES, 4u, 4u, kGreen);
    FillSolid(frames + 2u * SOLID_BYTES, 4u, 4u, kBlue);
    static const uint16_t durations[3] = {40u, 120u, 60u};
    wchar_t *path = paths->slots[0];
    MainFile(paths, path, PATH_CAP, L"v2", L".lt");
    WriteLtV2(path, frames, NULL, 4u, 4u, 3u, durations, 0x0123456789ABCDEFull, 4321u);

    static const wchar_t *const names[1] = {L"main/v2"};
    TexturePackData pack;
    Expect(Build(catalog, names, 1u, &pack) == TEXTURE_PACK_LOAD_OK, "v2 pack status");
    Expect((uint32_t)pack.sliceCount == 3u && (uint32_t)pack.animation[0].frameCount == 3u,
           "v2 frame count");
    Expect(pack.animation[0].cycleMilliseconds == 220u, "v2 cycle is the sum of delays");
    Expect((uint32_t)pack.sliceMilliseconds[0] == 40u &&
               (uint32_t)pack.sliceMilliseconds[1] == 120u &&
               (uint32_t)pack.sliceMilliseconds[2] == 60u,
           "v2 per-frame delays must survive the two passes");
    Expect(TexelEquals(AlbedoMip0(&pack, 0u), kRed) && TexelEquals(AlbedoMip0(&pack, 1u), kGreen) &&
               TexelEquals(AlbedoMip0(&pack, 2u), kBlue),
           "v2 frames must stay in order");
    ReleasePack(&pack);
}

static TEXTURE_TEST_NOINLINE void TestSingleTextureV2Normals(LaiueContentCatalog *catalog,
                                                             TestPaths *paths)
{
    uint8_t albedo[SOLID_BYTES];
    uint8_t normal[SOLID_BYTES];
    FillSolid(albedo, 4u, 4u, kRed);
    FillSolid(normal, 4u, 4u, kBlue);
    wchar_t *path = paths->slots[0];
    MainFile(paths, path, PATH_CAP, L"embed", L".lt");
    WriteLtV2(path, albedo, normal, 4u, 4u, 1u, NULL, 0u, 0u);

    static const wchar_t *const names[1] = {L"main/embed"};
    TexturePackData pack;
    Expect(Build(catalog, names, 1u, &pack) == TEXTURE_PACK_LOAD_OK, "embedded normal status");
    Expect(pack.normalPixels != NULL, "an embedded normal map must reach the pack");
    Expect(TexelEquals(NormalMip0(&pack, 0u), kBlue), "embedded normal pixels");
    ReleasePack(&pack);
}

// === Карта нормалей отдельным файлом ===

static TEXTURE_TEST_NOINLINE void TestNormalGeometry(LaiueContentCatalog *catalog, TestPaths *paths)
{
    // Совпадающая геометрия: карта обязана попасть в пак.
    wchar_t *albedoPath = paths->slots[0];
    wchar_t *normalPath = paths->slots[1];
    MainFile(paths, albedoPath, PATH_CAP, L"nm_match", L".png");
    MainFile(paths, normalPath, PATH_CAP, L"nm_match.normal", L".png");
    Expect(PlatformWriteEntireFile(albedoPath, PNG_SOLID_FILE, sizeof(PNG_SOLID_FILE)),
           "matching albedo write");
    Expect(PlatformWriteEntireFile(normalPath, PNG_RGBA_FILE, sizeof(PNG_RGBA_FILE)),
           "matching normal write");

    static const wchar_t *const matched[1] = {L"main/nm_match"};
    TexturePackData pack;
    Expect(Build(catalog, matched, 1u, &pack) == TEXTURE_PACK_LOAD_OK, "matching normal status");
    Expect(pack.normalPixels != NULL, "a matching normal map must not be dropped");
    Expect(memcmp(NormalMip0(&pack, 0u), PNG_RGBA_RGBA, 4u) == 0,
           "the normal map pixels must be the ones next to the albedo");
    Expect(TexelEquals(AlbedoMip0(&pack, 0u), kGreen), "matching normal albedo");
    ReleasePack(&pack);

    // Несовпадающая геометрия: 8x8 при альбедо 4x4 обязана быть отброшена,
    // иначе менялись бы и геометрия пака, и его байты.
    MainFile(paths, albedoPath, PATH_CAP, L"nm_mismatch", L".png");
    MainFile(paths, normalPath, PATH_CAP, L"nm_mismatch.normal", L".png");
    Expect(PlatformWriteEntireFile(albedoPath, PNG_SOLID_FILE, sizeof(PNG_SOLID_FILE)),
           "mismatched albedo write");
    Expect(PlatformWriteEntireFile(normalPath, PNG_INTERLACED_FILE, sizeof(PNG_INTERLACED_FILE)),
           "mismatched normal write");

    static const wchar_t *const mismatched[1] = {L"main/nm_mismatch"};
    Expect(Build(catalog, mismatched, 1u, &pack) == TEXTURE_PACK_LOAD_OK,
           "mismatched normal status");
    Expect((uint32_t)pack.width == 4u && pack.normalPixels == NULL,
           "a normal map of the wrong size must not change the pack");
    Expect(TexelEquals(AlbedoMip0(&pack, 0u), kGreen), "mismatched normal albedo");
    ReleasePack(&pack);
}

// === Отсутствующий материал ===

static TEXTURE_TEST_NOINLINE void TestMissingMaterial(LaiueContentCatalog *catalog)
{
    static const wchar_t *const names[1] = {L"main/absent"};
    TexturePackData pack;
    Expect(Build(catalog, names, 1u, &pack) == TEXTURE_PACK_LOAD_INCOMPLETE,
           "a missing material must be incomplete, not a failure");
    Expect((uint32_t)pack.sliceCount == 1u && (uint32_t)pack.materialCount == 1u &&
               (uint32_t)pack.animation[0].firstSlice == 0u &&
               (uint32_t)pack.animation[0].frameCount == 1u,
           "a missing material still occupies one layer");
    Expect(pack.normalPixels == NULL, "a missing material has no normal layer");
    Expect(TexelEquals(AlbedoMip0(&pack, 0u), kMissing),
           "a missing material must show the neutral grey");
    ReleasePack(&pack);
}

// === Кэш ===

static TEXTURE_TEST_NOINLINE void TestStaleCache(LaiueContentCatalog *catalog, TestPaths *paths)
{
    wchar_t *source = paths->slots[0];
    wchar_t *cache = paths->slots[1];
    MainFile(paths, source, PATH_CAP, L"stale", L".png");
    Expect(Join(cache, PATH_CAP, paths->main, L"stale.png.lt"), "stale cache path");

    Expect(PlatformWriteEntireFile(source, PNG_SOLID_FILE, sizeof(PNG_SOLID_FILE)),
           "stale source write");
    uint8_t red[SOLID_BYTES];
    FillSolid(red, 4u, 4u, kRed);
    // Отпечаток не совпадает с исходником: размер и время заведомо чужие.
    WriteLtV2(cache, red, NULL, 4u, 4u, 1u, NULL, 1u, 1u);

    static const wchar_t *const names[1] = {L"main/stale"};
    TexturePackData pack;
    Expect(Build(catalog, names, 1u, &pack) == TEXTURE_PACK_LOAD_OK, "stale cache status");
    Expect(TexelEquals(AlbedoMip0(&pack, 0u), kGreen),
           "a cache with the wrong fingerprint must not hide a live source");
    ReleasePack(&pack);
    Expect(PlatformPathExists(cache), "a rebuilt cache must be written back");
    // Из-за того же опечатка обязана откатиться: теперь рядом с источником
    // лежит уже правильный кэш.
    Expect(PlatformDeleteFile(source), "stale source removal");
    Expect(Build(catalog, names, 1u, &pack) == TEXTURE_PACK_LOAD_OK, "stale rebuilt cache status");
    Expect(TexelEquals(AlbedoMip0(&pack, 0u), kGreen),
           "the rewritten cache must round-trip the source exactly");
    ReleasePack(&pack);
}

static TEXTURE_TEST_NOINLINE void TestCorruptCache(LaiueContentCatalog *catalog, TestPaths *paths)
{
    wchar_t *source = paths->slots[0];
    wchar_t *cache = paths->slots[1];
    MainFile(paths, source, PATH_CAP, L"corrupt", L".png");
    Expect(Join(cache, PATH_CAP, paths->main, L"corrupt.png.lt"), "corrupt cache path");

    Expect(PlatformWriteEntireFile(source, PNG_SOLID_FILE, sizeof(PNG_SOLID_FILE)),
           "corrupt source write");
    static const uint8_t rubbish[64] = {0};
    Expect(PlatformWriteEntireFile(cache, rubbish, sizeof(rubbish)), "corrupt cache write");

    static const wchar_t *const names[1] = {L"main/corrupt"};
    TexturePackData pack;
    Expect(Build(catalog, names, 1u, &pack) == TEXTURE_PACK_LOAD_OK, "corrupt cache status");
    Expect(TexelEquals(AlbedoMip0(&pack, 0u), kGreen),
           "an unreadable cache must fall back to its source");
    ReleasePack(&pack);

    Expect(PlatformDeleteFile(source), "corrupt source removal");
    Expect(Build(catalog, names, 1u, &pack) == TEXTURE_PACK_LOAD_OK,
           "rebuilt corrupt cache status");
    Expect(TexelEquals(AlbedoMip0(&pack, 0u), kGreen),
           "the cache rebuilt over a corrupt one must be valid");
    ReleasePack(&pack);
}

// === Порядок форматов ===

static TEXTURE_TEST_NOINLINE void TestFormatOrder(LaiueContentCatalog *catalog, TestPaths *paths)
{
    wchar_t *source = paths->slots[0];
    wchar_t *authored = paths->slots[1];
    MainFile(paths, source, PATH_CAP, L"order", L".png");
    MainFile(paths, authored, PATH_CAP, L"order", L".lt");
    Expect(PlatformWriteEntireFile(source, PNG_SOLID_FILE, sizeof(PNG_SOLID_FILE)),
           "order png write");
    uint8_t red[SOLID_BYTES];
    FillSolid(red, 4u, 4u, kRed);
    WriteLtV2(authored, red, NULL, 4u, 4u, 1u, NULL, 0u, 0u);

    wchar_t *cache = paths->slots[2];
    Expect(Join(cache, PATH_CAP, paths->main, L"order.png.lt"), "order cache path");

    static const wchar_t *const names[1] = {L"main/order"};
    TexturePackData pack;

    // По умолчанию исходник старше своего `.lt`: показывается картинка.
    Expect(Build(catalog, names, 1u, &pack) == TEXTURE_PACK_LOAD_OK, "default order status");
    Expect(TexelEquals(AlbedoMip0(&pack, 0u), kGreen),
           "a live source must win over an authored .lt by default");
    ReleasePack(&pack);
    Expect(PlatformPathExists(cache), "reading a source must leave its cache");

    // `formats.txt` переставляет приоритет: свой формат первым берётся как
    // есть, и рядом с источником ничего не создаётся.
    Expect(PlatformDeleteFile(cache), "default cache removal");
    static const char ownFirst[] = "lt\n";
    Expect(PlatformWriteEntireFile(paths->formats, ownFirst, sizeof(ownFirst) - 1u),
           "formats.txt own-first write");
    Expect(Build(catalog, names, 1u, &pack) == TEXTURE_PACK_LOAD_OK, "own-first order status");
    Expect(TexelEquals(AlbedoMip0(&pack, 0u), kRed),
           "formats.txt must be able to put the engine format first");
    Expect(!PlatformPathExists(cache),
           "nothing may be built next to a source the engine never read");
    ReleasePack(&pack);

    // Формат, которого в файле нет, не исчезает: он идёт следом. Одна
    // забытая строка не должна прятать содержимое.
    Expect(PlatformDeleteFile(authored), "authored texture removal");
    static const char withoutPng[] = "gif\n";
    Expect(PlatformWriteEntireFile(paths->formats, withoutPng, sizeof(withoutPng) - 1u),
           "formats.txt without-png write");
    Expect(Build(catalog, names, 1u, &pack) == TEXTURE_PACK_LOAD_OK, "unlisted format status");
    Expect(TexelEquals(AlbedoMip0(&pack, 0u), kGreen),
           "a format missing from formats.txt must still be reachable");
    ReleasePack(&pack);
    Expect(PlatformDeleteFile(paths->formats), "formats.txt cleanup");
}

// === Холодная и тёплая сборка ===

static TEXTURE_TEST_NOINLINE void TestColdWarmEquality(LaiueContentCatalog *catalog,
                                                       TestPaths *paths)
{
    wchar_t *png = paths->slots[0];
    wchar_t *gif = paths->slots[1];
    wchar_t *authored = paths->slots[2];
    MainFile(paths, png, PATH_CAP, L"cw_png", L".png");
    MainFile(paths, gif, PATH_CAP, L"cw_gif", L".gif");
    MainFile(paths, authored, PATH_CAP, L"cw_v2", L".lt");
    Expect(PlatformWriteEntireFile(png, PNG_SOLID_FILE, sizeof(PNG_SOLID_FILE)), "cw png write");
    Expect(PlatformWriteEntireFile(gif, GIF_VARIABLE_FILE, sizeof(GIF_VARIABLE_FILE)),
           "cw gif write");

    uint8_t ltFrames[2u * SOLID_BYTES];
    FillSolid(ltFrames + 0u * SOLID_BYTES, 4u, 4u, kMagenta);
    FillSolid(ltFrames + 1u * SOLID_BYTES, 4u, 4u, kGreen);
    static const uint16_t ltDurations[2] = {10u, 20u};
    WriteLtV2(authored, ltFrames, NULL, 4u, 4u, 2u, ltDurations, 0u, 0u);

    static const wchar_t *const names[4] = {
        L"main/cw_png",
        L"main/cw_gif",
        L"main/cw_v2",
        L"main/cw_absent",
    };

    TexturePackData cold;
    Expect(Build(catalog, names, 4u, &cold) == TEXTURE_PACK_LOAD_INCOMPLETE,
           "cold mixed pack status");
    // Порядок слоёв: png(1) + gif(3) + lt(2) + отсутствующий(1).
    Expect((uint32_t)cold.sliceCount == 7u && (uint32_t)cold.materialCount == 4u,
           "cold mixed pack geometry");
    Expect((uint32_t)cold.animation[0].firstSlice == 0u &&
               (uint32_t)cold.animation[0].frameCount == 1u,
           "cold png layer");
    Expect((uint32_t)cold.animation[1].firstSlice == 1u &&
               (uint32_t)cold.animation[1].frameCount == 3u &&
               cold.animation[1].cycleMilliseconds == 220u,
           "cold animated gif layer");
    Expect((uint32_t)cold.sliceMilliseconds[1] == 40u &&
               (uint32_t)cold.sliceMilliseconds[2] == 120u &&
               (uint32_t)cold.sliceMilliseconds[3] == 60u,
           "gif frame delays must not be averaged");
    Expect((uint32_t)cold.animation[2].firstSlice == 4u &&
               (uint32_t)cold.animation[2].frameCount == 2u &&
               cold.animation[2].cycleMilliseconds == 30u,
           "cold engine format layer");
    Expect((uint32_t)cold.animation[3].firstSlice == 6u &&
               (uint32_t)cold.animation[3].frameCount == 1u,
           "cold missing layer");
    Expect(TexelEquals(AlbedoMip0(&cold, 0u), kGreen), "cold png pixels");
    Expect(TexelEquals(AlbedoMip0(&cold, 1u), kRed) && TexelEquals(AlbedoMip0(&cold, 4u), kMagenta),
           "cold first frames");

    PackSnapshot *coldSnapshot = PlatformAllocate(sizeof(*coldSnapshot), false);
    Expect(coldSnapshot != NULL, "cold snapshot allocation");
    SnapshotCapture(&cold, coldSnapshot);
    ReleasePack(&cold);

    wchar_t *pngCache = paths->slots[3];
    wchar_t *gifCache = paths->slots[4];
    Expect(Join(pngCache, PATH_CAP, paths->main, L"cw_png.png.lt") &&
               Join(gifCache, PATH_CAP, paths->main, L"cw_gif.gif.lt"),
           "cw cache paths");
    Expect(PlatformPathExists(pngCache) && PlatformPathExists(gifCache),
           "reading a foreign format must leave a prepared texture");

    // Тёплая сборка идёт уже без исходников: если бы кэш не был прочитан,
    // цвета стали бы нейтральными и сравнение с холодной сборкой упало бы.
    Expect(PlatformDeleteFile(png) && PlatformDeleteFile(gif), "cw source removal");
    TexturePackData warm;
    Expect(Build(catalog, names, 4u, &warm) == TEXTURE_PACK_LOAD_INCOMPLETE,
           "warm mixed pack status");
    PackSnapshot *warmSnapshot = PlatformAllocate(sizeof(*warmSnapshot), false);
    Expect(warmSnapshot != NULL, "warm snapshot allocation");
    SnapshotCapture(&warm, warmSnapshot);
    ReleasePack(&warm);

    Expect(SnapshotEquals(coldSnapshot, warmSnapshot),
           "cold and warm builds must produce the same pack byte for byte");

    SnapshotRelease(coldSnapshot);
    SnapshotRelease(warmSnapshot);
    PlatformFree(coldSnapshot);
    PlatformFree(warmSnapshot);
}

LAIUE_TEST_ENTRY(TexturePackBuildTestEntryPoint)
{
    TestPaths *paths = PlatformAllocate(sizeof(*paths), true);
    Expect(paths != NULL, "path scratch allocation");

    wchar_t executable[PATH_CAP];
    Expect(PlatformExecutableDirectory(executable, PATH_CAP), "executable directory");
    Expect(Join(paths->root, PATH_CAP, executable, TEST_ROOT_NAME) &&
               Join(paths->textures, PATH_CAP, paths->root, L"textures") &&
               Join(paths->pack, PATH_CAP, paths->textures, TEST_PACK_NAME) &&
               Join(paths->main, PATH_CAP, paths->pack, L"main") &&
               Join(paths->formats, PATH_CAP, paths->textures, L"formats.txt"),
           "path construction");
    Expect(PlatformCreateDirectory(paths->root) && PlatformCreateDirectory(paths->textures) &&
               PlatformCreateDirectory(paths->pack) && PlatformCreateDirectory(paths->main),
           "directory creation");
    // Прерванный прогон не должен оставлять чужую настройку форматов.
    PlatformDeleteFile(paths->formats);

    LaiueContentCatalog *catalog = LaiueContentCatalogCreate(paths->root);
    Expect(catalog != NULL, "content catalog creation");
    Expect(LaiueContentCatalogSetActivePack(catalog, LAIUE_CONTENT_TEXTURE_PACK, TEST_PACK_NAME),
           "texture pack activation");

    TestSingleTextureV1(catalog, paths);
    TestSingleTextureV1Normals(catalog, paths);
    TestSingleTextureV2Durations(catalog, paths);
    TestSingleTextureV2Normals(catalog, paths);
    TestNormalGeometry(catalog, paths);
    TestMissingMaterial(catalog);
    TestStaleCache(catalog, paths);
    TestCorruptCache(catalog, paths);
    TestFormatOrder(catalog, paths);
    TestColdWarmEquality(catalog, paths);

    LaiueContentCatalogDestroy(catalog);
    PlatformFree(paths);
    LaiueTestRuntimeWrite("texture pack build test passed\n");
    LAIUE_TEST_SUCCESS();
}
