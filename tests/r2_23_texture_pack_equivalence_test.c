// Эквивалентность runtime-части текстурпака (`src/render/texture_pack.c`)
// прежним алгоритмам: побайтовое сравнение таблицы кадров, покадрового
// решения и полей субресурса с дословными копиями старых реализаций, плюс
// семантика fallback/успешной загрузки `TexturePackLoadActiveFrom`.
//
// `texture_pack.c` не экспортируется из `laiue_render`, поэтому тест
// компилирует настоящий файл (и нужный ему `texture_build.c`) в свой
// исполняемый файл. Новые экспорты не нужны, GPU не нужен.

#include "content/content_catalog.h"
#include "content/content_service.h"
#include "media/lt_encode.h"
#include "platform/system.h"
#include "render/content_provider.h"
#include "render/texture_pack_internal.h"
#include "test_runtime.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define TEST_ROOT L"r2_23_texture_pack_equiv_v1"
#define TEST_PACK L"Equiv.ltp"
#define PATH_CAP LAIUE_PLATFORM_PATH_CAPACITY

#if defined(_MSC_VER)
#define EQ_NOINLINE __declspec(noinline)
#else
#define EQ_NOINLINE __attribute__((noinline))
#endif

static void Expect(bool condition, const char *message)
{
    if (condition) return;
    LaiueTestRuntimeWrite("r2_23 texture pack equivalence failed: ");
    LaiueTestRuntimeWrite(message);
    LaiueTestRuntimeWrite("\n");
    LaiueTestRuntimeExit(1);
}

static void WriteUnsigned(uint64_t value)
{
    char digits[21];
    uint32_t length = 0u;
    if (value == 0u) digits[length++] = '0';
    while (value != 0u)
    {
        digits[length++] = (char)('0' + (value % 10u));
        value /= 10u;
    }
    for (uint32_t index = 0; index < length; ++index)
    {
        char one[2] = {digits[length - index - 1u], '\0'};
        LaiueTestRuntimeWrite(one);
    }
}

// === Дословные копии прежних реализаций (base commit) ===

static uint32_t RefResolveSlice(const TexturePackAnimationSet *set, uint32_t material,
                                double animationSeconds)
{
    if (set == NULL || material >= TEXTURE_PACK_MAX_LAYERS) return 0u;
    const TexturePackAnimation *animation = &set->animation[material];
    if (animation->frameCount <= 1u || animation->cycleMilliseconds == 0u)
    {
        return animation->firstSlice;
    }
    if (!(animationSeconds > 0.0)) animationSeconds = 0.0;
    double cycle = (double)animation->cycleMilliseconds;
    double milliseconds = animationSeconds * 1000.0;
    if (milliseconds >= cycle)
    {
        milliseconds -= cycle * (double)(uint64_t)(milliseconds / cycle);
    }
    uint32_t elapsed = (uint32_t)milliseconds;
    uint32_t accumulated = 0u;
    for (uint32_t frame = 0; frame < animation->frameCount; ++frame)
    {
        uint32_t slice = (uint32_t)animation->firstSlice + frame;
        if (slice >= TEXTURE_PACK_MAX_SLICES) break;
        accumulated += set->sliceMilliseconds[slice];
        if (elapsed < accumulated) return slice;
    }
    return (uint32_t)animation->firstSlice + animation->frameCount - 1u;
}

static void RefFillSliceTable(const TexturePackAnimationSet *set, double animationSeconds,
                              uint32_t *outSlices)
{
    if (outSlices == NULL) return;
    for (uint32_t word = 0; word < 16u; ++word) outSlices[word] = 0u;
    if (set == NULL || set->materialCount == 0u) return;
    uint32_t materialCount = set->materialCount;
    if (materialCount > TEXTURE_PACK_MAX_LAYERS) materialCount = TEXTURE_PACK_MAX_LAYERS;
    uint32_t slice = 0u;
    for (uint32_t material = 0; material < TEXTURE_PACK_MAX_LAYERS; ++material)
    {
        if (material < materialCount)
        {
            slice = RefResolveSlice(set, material, animationSeconds);
        }
        uint32_t packed = slice > 255u ? 255u : slice;
        outSlices[material >> 2] |= packed << ((material & 3u) * 8u);
    }
}

static bool RefGetSubresourceFrom(const TexturePackData *pack, const uint8_t *base, uint32_t slice,
                                  uint32_t mip, TexturePackSubresource *outSubresource)
{
    if (pack == NULL || outSubresource == NULL || base == NULL || slice >= pack->sliceCount ||
        mip >= pack->mipCount)
    {
        return false;
    }

    uint64_t bytesPerLayer = 0;
    uint32_t width = pack->width;
    uint32_t height = pack->height;
    for (uint32_t level = 0; level < pack->mipCount; ++level)
    {
        bytesPerLayer += (uint64_t)width * height * 4u;
        if (width > 1u) width >>= 1;
        if (height > 1u) height >>= 1;
    }

    uint64_t offset = bytesPerLayer * slice;
    width = pack->width;
    height = pack->height;
    for (uint32_t level = 0; level < mip; ++level)
    {
        offset += (uint64_t)width * height * 4u;
        if (width > 1u) width >>= 1;
        if (height > 1u) height >>= 1;
    }

    uint64_t byteCount = (uint64_t)width * height * 4u;
    if (offset + byteCount > pack->pixelBytes || byteCount > UINT32_MAX) return false;

    outSubresource->pixels = base + (size_t)offset;
    outSubresource->width = width;
    outSubresource->height = height;
    outSubresource->rowBytes = width * 4u;
    outSubresource->byteCount = (uint32_t)byteCount;
    return true;
}

static uint32_t NextRandom(uint32_t *state)
{
    *state = *state * 1664525u + 1013904223u;
    return *state;
}

// === Таблица кадров и решение ===

static double g_times[13];

static void FillTimes(void)
{
    g_times[0] = 0.0;
    g_times[1] = -1.0;
    g_times[2] = 1e-9;
    g_times[3] = 0.001234;
    g_times[4] = 0.999;
    g_times[5] = 1.0;
    g_times[6] = 0.04;
    g_times[7] = 0.641;
    g_times[8] = 12.5;
    g_times[9] = 1234.5;
    g_times[10] = 86400.0;
    g_times[11] = 1e9;
    g_times[12] = 1e15;
}

// Детерминированное расписание: materialCount 0..67, у каждого материала
// 1..6 кадров, firstSlice вплоть до 399, длительности 0..65535.
static void RandomSet(TexturePackAnimationSet *set, uint32_t *state)
{
    memset(set, 0, sizeof(*set));
    uint32_t materialCount = NextRandom(state) % 68u;
    set->materialCount = materialCount;
    uint32_t slice = 0u;
    uint32_t sliceCount = 0u;
    for (uint32_t material = 0; material < materialCount; ++material)
    {
        uint32_t frames = 1u + NextRandom(state) % 6u;
        uint32_t first = NextRandom(state) % 400u;
        set->animation[material].firstSlice = (uint16_t)first;
        set->animation[material].frameCount = (uint16_t)frames;
        uint32_t cycle = 0u;
        for (uint32_t frame = 0; frame < frames; ++frame)
        {
            uint32_t sliceIndex = (first + frame) % TEXTURE_PACK_MAX_SLICES;
            uint16_t duration = (uint16_t)(NextRandom(state) % 700u);
            set->sliceMilliseconds[sliceIndex] = duration;
            cycle += duration;
        }
        // Нулевой цикл тоже допустим: это ветка «кадр не решается».
        if ((NextRandom(state) & 7u) == 0u) cycle = 0u;
        set->animation[material].cycleMilliseconds = cycle;
        (void)slice;
        sliceCount += frames;
    }
    set->sliceCount = sliceCount > TEXTURE_PACK_MAX_SLICES ? TEXTURE_PACK_MAX_SLICES : sliceCount;
}

static EQ_NOINLINE void TestFillEquivalence(void)
{
    uint32_t state = 0x12345678u;
    uint32_t mismatches = 0u;
    for (uint32_t iteration = 0; iteration < 200000u; ++iteration)
    {
        TexturePackAnimationSet set;
        RandomSet(&set, &state);
        double seconds = g_times[iteration % 13u];
        uint32_t real[16];
        uint32_t reference[16];
        TexturePackFillSliceTable(&set, seconds, real);
        RefFillSliceTable(&set, seconds, reference);
        if (memcmp(real, reference, sizeof(real)) != 0)
        {
            ++mismatches;
        }
        // И покадровое решение на каждом слоте.
        for (uint32_t material = 0; material < TEXTURE_PACK_MAX_LAYERS && mismatches == 0u;
             ++material)
        {
            uint32_t a = TexturePackResolveSlice(&set, material, seconds);
            uint32_t b = RefResolveSlice(&set, material, seconds);
            if (a != b)
            {
                ++mismatches;
            }
        }
        if (mismatches != 0u) break;
    }
    if (mismatches != 0u)
    {
        LaiueTestRuntimeWrite("fill mismatches=");
        WriteUnsigned(mismatches);
        LaiueTestRuntimeWrite("\n");
    }
    Expect(mismatches == 0u, "fill slice table must match the reference byte for byte");

    // Специальные случаи: NULL/nonzero проверки.
    uint32_t out[16];
    TexturePackFillSliceTable(NULL, 1.0, out);
    for (uint32_t word = 0; word < 16u; ++word) Expect(out[word] == 0u, "null set must zero");
    TexturePackAnimationSet empty;
    memset(&empty, 0, sizeof(empty));
    TexturePackFillSliceTable(&empty, 1.0, out);
    for (uint32_t word = 0; word < 16u; ++word) Expect(out[word] == 0u, "no materials must zero");
    Expect(TexturePackResolveSlice(NULL, 3u, 5.0) == 0u, "null set resolve");
    Expect(TexturePackResolveSlice(&empty, TEXTURE_PACK_MAX_LAYERS, 5.0) == 0u,
           "out-of-range material resolve");
}

// === Субресурс ===

static EQ_NOINLINE bool SubresourceEquals(const TexturePackSubresource *left,
                                          const TexturePackSubresource *right, bool leftOk,
                                          bool rightOk)
{
    if (leftOk != rightOk) return false;
    if (!leftOk) return true;
    return left->pixels == right->pixels && left->width == right->width &&
           left->height == right->height && left->rowBytes == right->rowBytes &&
           left->byteCount == right->byteCount;
}

static uint32_t MipLevels(uint32_t width, uint32_t height)
{
    uint32_t count = 1u;
    uint32_t probe = width > height ? width : height;
    while (probe > 1u)
    {
        probe >>= 1;
        ++count;
    }
    return count;
}

static uint64_t LayerBytes(uint32_t width, uint32_t height, uint32_t mipCount)
{
    uint64_t total = 0u;
    for (uint32_t level = 0; level < mipCount; ++level)
    {
        total += (uint64_t)width * height * 4u;
        if (width > 1u) width >>= 1;
        if (height > 1u) height >>= 1;
    }
    return total;
}

// Сравнивает реальный и эталонный субресурс по всем (slice, mip), включая
// выход за границы и усечённый pixelBytes.
static uint32_t CompareSubresources(const TexturePackData *pack, const uint8_t *pixels,
                                    const uint8_t *normals)
{
    uint32_t mismatches = 0u;
    for (uint32_t slice = 0; slice <= (uint32_t)pack->sliceCount; ++slice)
    {
        for (uint32_t level = 0; level <= (uint32_t)pack->mipCount; ++level)
        {
            TexturePackSubresource real;
            TexturePackSubresource reference;
            bool realOk = TexturePackGetSubresource(pack, slice, level, &real);
            bool refOk = RefGetSubresourceFrom(pack, pixels, slice, level, &reference);
            if (!SubresourceEquals(&real, &reference, realOk, refOk)) ++mismatches;
            realOk = TexturePackGetNormalSubresource(pack, slice, level, &real);
            refOk = RefGetSubresourceFrom(pack, normals, slice, level, &reference);
            if (!SubresourceEquals(&real, &reference, realOk, refOk)) ++mismatches;
        }
    }
    return mismatches;
}

static EQ_NOINLINE void TestSubresourceEquivalence(void)
{
    static const uint32_t sizes[9] = {1u, 2u, 3u, 4u, 5u, 8u, 64u, 255u, 256u};
    uint32_t mismatches = 0u;
    const uint32_t bufferBytes = 1u << 20;
    uint8_t *pixels = PlatformAllocate(bufferBytes, false);
    uint8_t *normals = PlatformAllocate(bufferBytes, false);
    Expect(pixels != NULL && normals != NULL, "subresource buffers");
    for (uint32_t index = 0; index < bufferBytes; ++index)
    {
        pixels[index] = (uint8_t)index;
        normals[index] = (uint8_t)(index * 7u);
    }
    for (uint32_t config = 0; config < 9u; ++config)
    {
        for (uint32_t aspect = 0; aspect < 3u; ++aspect)
        {
            TexturePackData pack;
            memset(&pack, 0, sizeof(pack));
            uint32_t width = sizes[config];
            uint32_t height = aspect == 0u ? sizes[config]
                              : aspect == 1u ? sizes[config] / 2u + 1u
                                             : 1u;
            uint32_t mip = MipLevels(width, height);
            uint64_t bytesPerLayer = LayerBytes(width, height, mip);
            uint32_t slices = sizes[config] % 5u + 1u;
            // Проверяем только конфигурации, целиком влезающие в буфер.
            if (bytesPerLayer * slices > bufferBytes) continue;
            pack.width = (uint16_t)width;
            pack.height = (uint16_t)height;
            pack.sliceCount = (uint16_t)slices;
            pack.mipCount = (uint16_t)mip;
            pack.pixels = pixels;
            pack.normalPixels = normals;
            pack.pixelBytes = (uint32_t)(bytesPerLayer * slices);
            mismatches += CompareSubresources(&pack, pixels, normals);
        }
    }

    // Граничный размер 4096 (13 mip-уровней): точная цепочка на один слой.
    {
        uint32_t mip = MipLevels(4096u, 4096u);
        uint64_t bytes = LayerBytes(4096u, 4096u, mip);
        Expect(bytes < 0xFFFFFFFFull, "4096 chain fits uint32");
        uint8_t *bigPixels = PlatformAllocate((size_t)bytes, false);
        uint8_t *bigNormals = PlatformAllocate((size_t)bytes, false);
        if (bigPixels != NULL && bigNormals != NULL)
        {
            for (size_t index = 0; index < (size_t)bytes; ++index)
            {
                bigPixels[index] = (uint8_t)(index * 3u);
                bigNormals[index] = (uint8_t)(index * 11u);
            }
            TexturePackData pack;
            memset(&pack, 0, sizeof(pack));
            pack.width = 4096u;
            pack.height = 4096u;
            pack.sliceCount = 1u;
            pack.mipCount = (uint16_t)mip;
            pack.pixels = bigPixels;
            pack.normalPixels = bigNormals;
            pack.pixelBytes = (uint32_t)bytes;
            mismatches += CompareSubresources(&pack, bigPixels, bigNormals);
        }
        PlatformFree(bigPixels);
        PlatformFree(bigNormals);
    }

    // Маленький pixelBytes: обе версии обязаны отказать.
    {
        TexturePackData pack;
        memset(&pack, 0, sizeof(pack));
        pack.width = 8u;
        pack.height = 8u;
        pack.sliceCount = 2u;
        pack.mipCount = 4u;
        pack.pixels = pixels;
        pack.normalPixels = normals;
        pack.pixelBytes = 15u;
        mismatches += CompareSubresources(&pack, pixels, normals);
    }

    if (mismatches != 0u)
    {
        LaiueTestRuntimeWrite("subresource mismatches=");
        WriteUnsigned(mismatches);
        LaiueTestRuntimeWrite("\n");
    }
    Expect(mismatches == 0u, "subresource fields must match the reference");
    PlatformFree(pixels);
    PlatformFree(normals);
}

// === Загрузка: успех и fallback ===

static bool Join(wchar_t *destination, uint32_t capacity, const wchar_t *base, const wchar_t *part)
{
    uint32_t length = 0u;
    while (base[length] != 0)
    {
        if (length + 1u >= capacity) return false;
        destination[length] = base[length];
        ++length;
    }
    if (length + 1u >= capacity) return false;
    destination[length++] = L'/';
    for (uint32_t index = 0u; part[index] != 0; ++index)
    {
        if (length + 1u >= capacity) return false;
        destination[length++] = part[index];
    }
    destination[length] = 0;
    return true;
}

static bool Append(wchar_t *destination, uint32_t capacity, const wchar_t *suffix)
{
    uint32_t length = 0u;
    while (destination[length] != 0) ++length;
    for (uint32_t index = 0u; suffix[index] != 0; ++index)
    {
        if (length + 1u >= capacity) return false;
        destination[length++] = suffix[index];
    }
    destination[length] = 0;
    return true;
}

static EQ_NOINLINE bool WriteLt(const wchar_t *path, uint32_t size, uint8_t fill)
{
    uint32_t frameBytes = size * size * 4u;
    uint8_t *pixels = PlatformAllocate(frameBytes, false);
    if (pixels == NULL) return false;
    for (uint32_t index = 0; index < frameBytes; ++index) pixels[index] = fill;
    uint32_t encodedBytes = 0u;
    if (LtEncodedBytes(size, size, 1u, false, &encodedBytes) != LT_OK)
    {
        PlatformFree(pixels);
        return false;
    }
    uint8_t *encoded = PlatformAllocate(encodedBytes, false);
    if (encoded == NULL)
    {
        PlatformFree(pixels);
        return false;
    }
    LtTexture texture = {.albedoFrames = pixels,
                         .normalFrames = NULL,
                         .width = size,
                         .height = size,
                         .frameCount = 1u,
                         .frameMilliseconds = NULL,
                         .sourceModifiedTime = 0u,
                         .sourceSizeBytes = 0u};
    bool ok = LtEncode(&texture, encoded, encodedBytes, NULL) == LT_OK &&
              PlatformWriteEntireFile(path, encoded, encodedBytes);
    PlatformFree(encoded);
    PlatformFree(pixels);
    return ok;
}

static EQ_NOINLINE void TestLoadSemantics(void)
{
    // Пути в статике: no-CRT сборка не линкует __chkstk, а шесть буферов
    // по 2 КиБ перевалили бы кадр точки входа.
    static wchar_t executable[PATH_CAP];
    static wchar_t root[PATH_CAP];
    static wchar_t textures[PATH_CAP];
    static wchar_t pack[PATH_CAP];
    static wchar_t main[PATH_CAP];
    static wchar_t file[PATH_CAP];
    Expect(PlatformExecutableDirectory(executable, PATH_CAP), "executable directory");
    Expect(Join(root, PATH_CAP, executable, TEST_ROOT) &&
               Join(textures, PATH_CAP, root, L"textures") &&
               Join(pack, PATH_CAP, textures, TEST_PACK) &&
               Join(main, PATH_CAP, pack, L"main"),
           "path construction");
    PlatformCreateDirectory(root);
    PlatformCreateDirectory(textures);
    PlatformCreateDirectory(pack);
    PlatformCreateDirectory(main);
    Expect(Join(file, PATH_CAP, main, L"solo") && Append(file, PATH_CAP, L".lt") &&
               WriteLt(file, 8u, 200u),
           "texture write");

    static const wchar_t *const names[1] = {L"main/solo"};
    LaiueContentCatalog *catalog = LaiueContentCatalogCreate(root);
    Expect(catalog != NULL, "catalog creation");
    // Прогон не должен зависеть от предыдущего: он оставляет active.txt.
    Expect(LaiueContentCatalogSetActivePack(catalog, LAIUE_CONTENT_TEXTURE_PACK, NULL),
           "active pack reset");

    // Нет активного пака: статус NO_ACTIVE_PACK, но рабочий нейтральный слой.
    TexturePackData fallback;
    memset(&fallback, 0xa5, sizeof(fallback));   // мусор: SetFallback обязан перекрыть поля
    TexturePackLoadStatus status = TexturePackLoadActiveFrom(catalog, names, 1u, &fallback);
    Expect(status == TEXTURE_PACK_LOAD_NO_ACTIVE_PACK, "fallback status");
    Expect(fallback.width == 1u && fallback.height == 1u && fallback.sliceCount == 1u &&
               fallback.mipCount == 1u && fallback.materialCount == 1u &&
               fallback.pixelBytes == 4u && fallback.pixels != NULL && fallback.allocation == NULL &&
               fallback.animation[0].frameCount == 1u,
           "fallback neutral layer");
    TexturePackRelease(&fallback);

    // Пустой каталог тоже даёт NO_ACTIVE_PACK.
    TexturePackData nullCatalog;
    status = TexturePackLoadActiveFrom(NULL, names, 1u, &nullCatalog);
    Expect(status == TEXTURE_PACK_LOAD_NO_ACTIVE_PACK, "null catalog status");
    TexturePackRelease(&nullCatalog);

    // Успешная загрузка обязана полностью перекрыть предыдущий fallback.
    Expect(LaiueContentCatalogSetActivePack(catalog, LAIUE_CONTENT_TEXTURE_PACK, TEST_PACK),
           "pack activation");
    TexturePackData loaded;
    memset(&loaded, 0xa5, sizeof(loaded));
    status = TexturePackLoadActiveFrom(catalog, names, 1u, &loaded);
    Expect(status == TEXTURE_PACK_LOAD_OK, "load status");
    Expect(loaded.width == 8u && loaded.height == 8u && loaded.sliceCount == 1u &&
               loaded.materialCount == 1u && loaded.pixels != NULL && loaded.allocation != NULL &&
               loaded.pixelBytes != 0u,
           "loaded pack fields");
    // Верхний левый тексель — залитый цвет.
    Expect(loaded.pixels[0] == 200u && loaded.pixels[1] == 200u && loaded.pixels[2] == 200u &&
               loaded.pixels[3] == 200u,
           "loaded pixels");
    TexturePackRelease(&loaded);
    Expect(loaded.pixels == NULL && loaded.allocation == NULL && loaded.pixelBytes == 0u,
           "release resets fields");

    LaiueContentCatalogDestroy(catalog);
}

LAIUE_TEST_ENTRY(R2TexturePackEquivalenceTestEntryPoint)
{
    RendererSetContentService(LaiueContentGetStaticServiceV1());
    FillTimes();
    TestFillEquivalence();
    TestSubresourceEquivalence();
    TestLoadSemantics();
    LaiueTestRuntimeWrite("r2_23 texture pack equivalence passed\n");
    LAIUE_TEST_SUCCESS();
}
