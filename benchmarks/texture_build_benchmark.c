// Ручной A/B-стенд сборки текстурного пака (`TexturePackBuildFrom`).
//
// В обычную сборку и в CTest не входит: включается LAIUE_BUILD_BENCHMARKS и
// запускается руками из A/B-скрипта. Стенд компилирует внутренний
// `src/graphics/render/texture_build.c` прямо в свой исполняемый файл: сборщик не
// экспортируется из `laiue_render`, а новые экспорты запрещены.
//
// Стенд сам готовит детерминированное содержимое — `.lt`-файлы без
// PNG/GIF/JPEG — и прогоняет четыре сценария, различающихся данными
// (статика, встроенные нормали, анимация с отдельной картой нормалей и
// анимация со встроенными нормалями). Каждый сценарий: прогрев, затем девять
// парных замеров full/base-only с чередованием порядка; печатается медиана.
// Checksum убеждается, что mip 0 и метаданные не изменились.
//
// Windows собирает движок без CRT, поэтому вывод — через общий с
// тестами `test_runtime.h`, а не через printf.

#include "content/content_catalog.h"
#include "media/lt_encode.h"
#include "platform/system.h"
#include "render/content_provider.h"
#include "render/texture_pack_internal.h"
#include "test_runtime.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define BENCH_ROOT_NAME L"texture_build_bench_v1"
#define BENCH_PACK_NAME L"Bench.ltp"
#define PATH_CAP LAIUE_PLATFORM_PATH_CAPACITY
// Локальный буфер пути материала: короче полного PATH_CAP, чтобы кадр
// функции оставался под пределом 4 КиБ и no-CRT сборке не требовался
// __chkstk.
#define FILE_CAP 512u

// No-CRT линковка не приносит __chkstk, поэтому крупные сценарии не
// позволяем LTO вклеить в точку входа и слить их кадры стека.
#if defined(_MSC_VER) || defined(__clang__)
#define BENCH_NOINLINE __declspec(noinline)
#else
#define BENCH_NOINLINE __attribute__((noinline))
#endif

// 256x256 — рабочая сторона текстурпака: уровень 0 цепочки 262 144 Б,
// вся цепочка 349 524 Б. Меньше — шум замера начинает перевешивать
// интересующий эффект.
#define TEX_SIZE 256u
#define FRAME_BYTES (TEX_SIZE * TEX_SIZE * 4u)

#define STATIC_COUNT 32u
#define NORMAL_COUNT 32u
#define ANIM_COUNT 8u
#define ANIM_FRAMES 8u
#define TOTAL_COUNT (STATIC_COUNT + NORMAL_COUNT + ANIM_COUNT + ANIM_COUNT)
#define BENCH_SAMPLES 9u

typedef struct BenchPaths
{
    wchar_t root[PATH_CAP];
    wchar_t textures[PATH_CAP];
    wchar_t pack[PATH_CAP];
    wchar_t main[PATH_CAP];
    wchar_t formats[PATH_CAP];
    // Рабочие буферы путей тоже в куче: no-CRT сборке не нужен __chkstk,
    // а кадр PrepareContent не должен приближаться к пределу 4 КиБ.
    wchar_t executable[PATH_CAP];
    wchar_t file[FILE_CAP];
    wchar_t normalName[LAIUE_CONTENT_NAME_CAPACITY];
    // Имена материалов вида `main/s00`: ровно то, что приложение передаёт
    // сборщику.
    wchar_t names[TOTAL_COUNT][LAIUE_CONTENT_NAME_CAPACITY];
} BenchPaths;

// Одна длительность на кадр. Значение не влияет на пиксели; важно лишь,
// чтобы у анимации каждый кадр был ненулевым.
static uint16_t g_durations[256];

static void WriteText(const char *text)
{
    LaiueTestRuntimeWrite(text);
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
        WriteText(one);
    }
}

static void WriteMilliseconds(double value)
{
    if (value < 0.0) value = 0.0;
    uint64_t thousandths = (uint64_t)(value * 1000.0 + 0.5);
    WriteUnsigned(thousandths / 1000u);
    WriteText(".");
    uint64_t fraction = thousandths % 1000u;
    if (fraction < 100u) WriteText("0");
    if (fraction < 10u) WriteText("0");
    WriteUnsigned(fraction);
}

static void WriteHex(uint64_t value)
{
    static const char digits[] = "0123456789abcdef";
    char text[17];
    for (uint32_t index = 0; index < 16u; ++index)
    {
        text[index] = digits[(value >> ((15u - index) * 4u)) & 0xFu];
    }
    text[16] = '\0';
    WriteText(text);
}

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

// `main/<letter><two digits>`: имя ресурса внутри пака.
static void FormatName(wchar_t *destination, uint32_t capacity, wchar_t group, uint32_t index)
{
    destination[0] = L'm';
    destination[1] = L'a';
    destination[2] = L'i';
    destination[3] = L'n';
    destination[4] = L'/';
    destination[5] = group;
    destination[6] = (wchar_t)(L'0' + (index / 10u) % 10u);
    destination[7] = (wchar_t)(L'0' + index % 10u);
    destination[8] = 0;
    (void)capacity;
}

static void FillPattern(uint8_t *bytes, uint32_t count, uint32_t seed)
{
    for (uint32_t index = 0; index < count; ++index)
    {
        bytes[index] = (uint8_t)((index * 31u + seed * 7u) & 0xFFu);
    }
}

// Пишет один `.lt` ровно через тот же кодировщик, что и движок.
static BENCH_NOINLINE bool WriteTexture(const wchar_t *path, uint32_t width, uint32_t height,
                                        uint32_t frameCount, bool withNormals, uint32_t seed)
{
    uint32_t frameBytes = width * height * 4u;
    uint32_t albedoBytes = frameBytes * frameCount;
    uint8_t *albedo = PlatformAllocate(albedoBytes, false);
    uint8_t *normal = withNormals ? PlatformAllocate(albedoBytes, false) : NULL;
    if (albedo == NULL || (withNormals && normal == NULL))
    {
        PlatformFree(albedo);
        PlatformFree(normal);
        return false;
    }
    for (uint32_t frame = 0; frame < frameCount; ++frame)
    {
        FillPattern(albedo + (size_t)frame * frameBytes, frameBytes, seed + frame);
        if (normal != NULL) FillPattern(normal + (size_t)frame * frameBytes, frameBytes, seed + 1000u + frame);
    }
    for (uint32_t frame = 0; frame < frameCount; ++frame) g_durations[frame] = 40u;

    uint32_t encodedBytes = 0u;
    if (LtEncodedBytes(width, height, frameCount, withNormals, &encodedBytes) != LT_OK)
    {
        PlatformFree(albedo);
        PlatformFree(normal);
        return false;
    }
    uint8_t *encoded = PlatformAllocate(encodedBytes, false);
    if (encoded == NULL)
    {
        PlatformFree(albedo);
        PlatformFree(normal);
        return false;
    }
    LtTexture texture = {
        .albedoFrames = albedo,
        .normalFrames = normal,
        .width = width,
        .height = height,
        .frameCount = frameCount,
        .frameMilliseconds = g_durations,
        .sourceModifiedTime = 0u,
        .sourceSizeBytes = 0u,
    };
    bool ok = LtEncode(&texture, encoded, encodedBytes, NULL) == LT_OK &&
              PlatformWriteEntireFile(path, encoded, encodedBytes);
    PlatformFree(encoded);
    PlatformFree(albedo);
    PlatformFree(normal);
    return ok;
}

static uint64_t HashBytes(uint64_t hash, const uint8_t *bytes, uint32_t count)
{
    for (uint32_t index = 0; index < count; ++index)
    {
        hash ^= bytes[index];
        hash *= 1099511628211ull;
    }
    return hash;
}

static BENCH_NOINLINE uint64_t HashPack(const TexturePackData *pack)
{
    uint64_t hash = 1469598103934665603ull;
    hash = HashBytes(hash, pack->pixels, pack->pixelBytes);
    if (pack->normalPixels != NULL)
    {
        hash = HashBytes(hash, pack->normalPixels, pack->pixelBytes);
    }
    hash = HashBytes(hash, (const uint8_t *)pack->animation, (uint32_t)sizeof(pack->animation));
    hash = HashBytes(hash, (const uint8_t *)pack->sliceMilliseconds,
                    (uint32_t)sizeof(pack->sliceMilliseconds));
    return hash;
}

static BENCH_NOINLINE uint64_t HashBaseLevels(const TexturePackData *pack)
{
    uint64_t hash = 1469598103934665603ull;
    uint32_t mip0Bytes = (uint32_t)pack->width * pack->height * 4u;
    uint32_t fullSliceBytes = 0u;
    uint32_t mipSize = pack->width;
    for (uint32_t mip = 0u; mip < pack->mipCount; ++mip)
    {
        fullSliceBytes += mipSize * mipSize * 4u;
        if (mipSize > 1u) mipSize >>= 1;
    }
    for (uint32_t slice = 0u; slice < pack->sliceCount; ++slice)
        hash = HashBytes(hash, pack->pixels + (size_t)slice * fullSliceBytes, mip0Bytes);
    if (pack->normalPixels != NULL)
    {
        for (uint32_t slice = 0u; slice < pack->sliceCount; ++slice)
            hash = HashBytes(hash, pack->normalPixels + (size_t)slice * fullSliceBytes,
                             mip0Bytes);
    }
    hash = HashBytes(hash, (const uint8_t *)pack->animation, (uint32_t)sizeof(pack->animation));
    hash = HashBytes(hash, (const uint8_t *)pack->sliceMilliseconds,
                     (uint32_t)sizeof(pack->sliceMilliseconds));
    return hash;
}

static void ReleasePack(TexturePackData *pack)
{
    PlatformFree(pack->allocation);
    memset(pack, 0, sizeof(*pack));
}

static BENCH_NOINLINE TexturePackLoadStatus Build(LaiueContentCatalog *catalog,
                                                  const wchar_t *const *names, uint32_t count,
                                                  TexturePackData *outPack)
{
    memset(outPack, 0, sizeof(*outPack));
    return TexturePackBuildFrom(catalog, names, count, outPack);
}

static BENCH_NOINLINE TexturePackLoadStatus BuildBaseLevel(
    LaiueContentCatalog *catalog, const wchar_t *const *names, uint32_t count,
    TexturePackData *outPack)
{
    memset(outPack, 0, sizeof(*outPack));
    return TexturePackBuildBaseLevelFrom(catalog, names, count, outPack);
}

typedef TexturePackLoadStatus (*TexturePackBuilder)(
    LaiueContentCatalog *, const wchar_t *const *, uint32_t, TexturePackData *);

typedef struct PackSummary
{
    uint16_t width;
    uint16_t height;
    uint16_t mipCount;
    uint16_t sliceCount;
    uint16_t materialCount;
    uint32_t pixelBytes;
    uint64_t totalPixelBytes;
    uint64_t checksum;
    uint64_t baseChecksum;
} PackSummary;

static BENCH_NOINLINE bool MeasureBuild(
    TexturePackBuilder builder, LaiueContentCatalog *catalog,
    const wchar_t *const *names, uint32_t count, double *outMilliseconds,
    PackSummary *outSummary)
{
    TexturePackData pack;
    memset(&pack, 0, sizeof(pack));
    double start = PlatformMonotonicSeconds();
    TexturePackLoadStatus status = builder(catalog, names, count, &pack);
    *outMilliseconds = (PlatformMonotonicSeconds() - start) * 1000.0;
    if (status != TEXTURE_PACK_LOAD_OK && status != TEXTURE_PACK_LOAD_INCOMPLETE)
    {
        ReleasePack(&pack);
        return false;
    }

    outSummary->width = pack.width;
    outSummary->height = pack.height;
    outSummary->mipCount = pack.mipCount;
    outSummary->sliceCount = pack.sliceCount;
    outSummary->materialCount = pack.materialCount;
    outSummary->pixelBytes = pack.pixelBytes;
    outSummary->totalPixelBytes = (uint64_t)pack.pixelBytes *
                                  (pack.normalPixels != NULL ? 2u : 1u);
    outSummary->checksum = HashPack(&pack);
    outSummary->baseChecksum = HashBaseLevels(&pack);
    ReleasePack(&pack);
    return true;
}

static void SortSamples(double *samples)
{
    for (uint32_t index = 1u; index < BENCH_SAMPLES; ++index)
    {
        double value = samples[index];
        uint32_t cursor = index;
        while (cursor > 0u && samples[cursor - 1u] > value)
        {
            samples[cursor] = samples[cursor - 1u];
            --cursor;
        }
        samples[cursor] = value;
    }
}

static void ReportMeasuredScenario(const char *label, double milliseconds,
                                   const PackSummary *summary, bool baseOnly, bool verified)
{
    WriteText("RESULT scenario=");
    WriteText(label);
    if (baseOnly) WriteText("_base");
    WriteText(" ms=");
    WriteMilliseconds(milliseconds);
    WriteText(" samples=");
    WriteUnsigned(BENCH_SAMPLES);
    WriteText(" checksum=");
    WriteHex(summary->checksum);
    WriteText(" width=");
    WriteUnsigned(summary->width);
    WriteText(" height=");
    WriteUnsigned(summary->height);
    WriteText(" mip=");
    WriteUnsigned(summary->mipCount);
    WriteText(" slices=");
    WriteUnsigned(summary->sliceCount);
    WriteText(" materials=");
    WriteUnsigned(summary->materialCount);
    WriteText(" pixelbytes=");
    WriteUnsigned(summary->pixelBytes);
    WriteText(" totalpixelbytes=");
    WriteUnsigned(summary->totalPixelBytes);
    WriteText(" base_checksum=");
    WriteHex(summary->baseChecksum);
    WriteText(" verify=");
    WriteText(verified ? "ok\n" : "fail\n");
}

static BENCH_NOINLINE bool PrepareContent(BenchPaths *paths)
{
    wchar_t *executable = paths->executable;
    if (!PlatformExecutableDirectory(executable, PATH_CAP)) return false;
    if (!Join(paths->root, PATH_CAP, executable, BENCH_ROOT_NAME) ||
        !Join(paths->textures, PATH_CAP, paths->root, L"textures") ||
        !Join(paths->pack, PATH_CAP, paths->textures, BENCH_PACK_NAME) ||
        !Join(paths->main, PATH_CAP, paths->pack, L"main") ||
        !Join(paths->formats, PATH_CAP, paths->textures, L"formats.txt"))
    {
        return false;
    }
    PlatformCreateDirectory(paths->root);
    PlatformCreateDirectory(paths->textures);
    PlatformCreateDirectory(paths->pack);
    PlatformCreateDirectory(paths->main);
    // Настройка форматов не должна переезжать из прошлого прогона: у
    // сценариев только `.lt`, и порядок не должен от неё зависеть.
    PlatformDeleteFile(paths->formats);

    wchar_t *file = paths->file;
    bool ok = true;
    for (uint32_t index = 0; index < STATIC_COUNT && ok; ++index)
    {
        FormatName(paths->names[index], LAIUE_CONTENT_NAME_CAPACITY, L's', index);
        if (!Join(file, FILE_CAP, paths->main, &paths->names[index][5]) || !Append(file, FILE_CAP, L".lt"))
        {
            ok = false;
            break;
        }
        ok = WriteTexture(file, TEX_SIZE, TEX_SIZE, 1u, false, index + 1u);
    }
    for (uint32_t index = 0; index < NORMAL_COUNT && ok; ++index)
    {
        uint32_t slot = STATIC_COUNT + index;
        FormatName(paths->names[slot], LAIUE_CONTENT_NAME_CAPACITY, L'n', index);
        if (!Join(file, FILE_CAP, paths->main, &paths->names[slot][5]) || !Append(file, FILE_CAP, L".lt"))
        {
            ok = false;
            break;
        }
        ok = WriteTexture(file, TEX_SIZE, TEX_SIZE, 1u, true, 200u + index);
    }
    for (uint32_t index = 0; index < ANIM_COUNT && ok; ++index)
    {
        uint32_t slot = STATIC_COUNT + NORMAL_COUNT + index;
        FormatName(paths->names[slot], LAIUE_CONTENT_NAME_CAPACITY, L'a', index);
        if (!Join(file, FILE_CAP, paths->main, &paths->names[slot][5]) || !Append(file, FILE_CAP, L".lt"))
        {
            ok = false;
            break;
        }
        ok = WriteTexture(file, TEX_SIZE, TEX_SIZE, ANIM_FRAMES, false, 400u + index);
        // Отдельная карта нормалей на один кадр: старый путь размножал её
        // на все кадры albedo, новый обязан обойтись одним кадром.
        if (ok)
        {
            wchar_t *normalName = paths->normalName;
            uint32_t length = 0u;
            while (paths->names[slot][length] != 0)
            {
                normalName[length] = paths->names[slot][length];
                ++length;
            }
            static const wchar_t suffix[] = L".normal";
            for (uint32_t part = 0u; suffix[part] != 0; ++part) normalName[length++] = suffix[part];
            normalName[length] = 0;
            if (!Join(file, FILE_CAP, paths->main, &normalName[5]) || !Append(file, FILE_CAP, L".lt"))
            {
                ok = false;
                break;
            }
            ok = WriteTexture(file, TEX_SIZE, TEX_SIZE, 1u, false, 600u + index);
        }
    }
    for (uint32_t index = 0; index < ANIM_COUNT && ok; ++index)
    {
        uint32_t slot = STATIC_COUNT + NORMAL_COUNT + ANIM_COUNT + index;
        FormatName(paths->names[slot], LAIUE_CONTENT_NAME_CAPACITY, L'e', index);
        if (!Join(file, FILE_CAP, paths->main, &paths->names[slot][5]) || !Append(file, FILE_CAP, L".lt"))
        {
            ok = false;
            break;
        }
        ok = WriteTexture(file, TEX_SIZE, TEX_SIZE, ANIM_FRAMES, true, 800u + index);
    }
    return ok;
}

static BENCH_NOINLINE void RunScenario(LaiueContentCatalog *catalog, const char *label,
                                       const wchar_t *const *names, uint32_t count)
{
    double fullSamples[BENCH_SAMPLES];
    double baseSamples[BENCH_SAMPLES];
    PackSummary fullSummary;
    PackSummary baseSummary;
    double ignoredMilliseconds = 0.0;
    if (!MeasureBuild(Build, catalog, names, count, &ignoredMilliseconds, &fullSummary) ||
        !MeasureBuild(BuildBaseLevel, catalog, names, count, &ignoredMilliseconds, &baseSummary))
    {
        WriteText("ERROR scenario=");
        WriteText(label);
        WriteText(" warmup\n");
        return;
    }

    bool verified = true;
    uint64_t expectedFullChecksum = fullSummary.checksum;
    uint64_t expectedBaseChecksum = fullSummary.baseChecksum;
    for (uint32_t sample = 0u; sample < BENCH_SAMPLES; ++sample)
    {
        // Alternate order to reduce clock and scheduler drift bias.
        bool baseFirst = (sample & 1u) != 0u;
        if (!baseFirst)
        {
            verified = MeasureBuild(Build, catalog, names, count, &fullSamples[sample],
                                    &fullSummary) && verified;
            if (!MeasureBuild(BuildBaseLevel, catalog, names, count, &baseSamples[sample],
                              &baseSummary)) verified = false;
        }
        else
        {
            if (!MeasureBuild(BuildBaseLevel, catalog, names, count, &baseSamples[sample],
                              &baseSummary)) verified = false;
            verified = MeasureBuild(Build, catalog, names, count, &fullSamples[sample],
                                    &fullSummary) && verified;
        }

        verified = verified && fullSummary.checksum == expectedFullChecksum &&
                   fullSummary.baseChecksum == expectedBaseChecksum &&
                   baseSummary.checksum == expectedBaseChecksum &&
                   baseSummary.baseChecksum == expectedBaseChecksum;
    }
    if (!verified)
    {
        WriteText("ERROR scenario=");
        WriteText(label);
        WriteText(" benchmark build or checksum mismatch\n");
        return;
    }

    SortSamples(fullSamples);
    SortSamples(baseSamples);
    bool match = baseSummary.checksum == fullSummary.baseChecksum;
    ReportMeasuredScenario(label, fullSamples[BENCH_SAMPLES / 2u], &fullSummary, false, match);
    ReportMeasuredScenario(label, baseSamples[BENCH_SAMPLES / 2u], &baseSummary, true, match);
}

LAIUE_TEST_ENTRY(TextureBuildBenchmarkEntryPoint)
{
    // Сборщик ходит в каталог контента через мост `RendererContent*`;
    // без установленного сервиса `TexturePackBuildFrom` сразу возвращает
    // NO_ACTIVE_PACK и стенд меряет fallback, а не сборку.
    RendererSetContentService(LaiueContentGetStaticServiceV1());
    BenchPaths *paths = PlatformAllocate(sizeof(*paths), true);
    if (paths == NULL)
    {
        WriteText("texture build benchmark: path allocation failed\n");
        LaiueTestRuntimeExit(1);
    }
    if (!PrepareContent(paths))
    {
        WriteText("texture build benchmark: content preparation failed\n");
        PlatformFree(paths);
        LaiueTestRuntimeExit(1);
    }

    LaiueContentCatalog *catalog = LaiueContentCatalogCreate(paths->root);
    if (catalog == NULL ||
        !LaiueContentCatalogSetActivePack(catalog, LAIUE_CONTENT_TEXTURE_PACK, BENCH_PACK_NAME))
    {
        WriteText("texture build benchmark: catalog setup failed\n");
        PlatformFree(paths);
        LaiueTestRuntimeExit(1);
    }

    const wchar_t *staticNames[STATIC_COUNT];
    const wchar_t *normalNames[NORMAL_COUNT];
    const wchar_t *animNames[ANIM_COUNT];
    const wchar_t *animEmbedNames[ANIM_COUNT];
    for (uint32_t index = 0; index < STATIC_COUNT; ++index)
        staticNames[index] = paths->names[index];
    for (uint32_t index = 0; index < NORMAL_COUNT; ++index)
        normalNames[index] = paths->names[STATIC_COUNT + index];
    for (uint32_t index = 0; index < ANIM_COUNT; ++index)
        animNames[index] = paths->names[STATIC_COUNT + NORMAL_COUNT + index];
    for (uint32_t index = 0; index < ANIM_COUNT; ++index)
        animEmbedNames[index] = paths->names[STATIC_COUNT + NORMAL_COUNT + ANIM_COUNT + index];

    WriteText("texture build benchmark paired samples=");
    WriteUnsigned(BENCH_SAMPLES);
    WriteText(" (ms is median wall time per build)\n");
    RunScenario(catalog, "lt_static", staticNames, STATIC_COUNT);
    RunScenario(catalog, "lt_normals", normalNames, NORMAL_COUNT);
    RunScenario(catalog, "lt_anim_separate_normal", animNames, ANIM_COUNT);
    RunScenario(catalog, "lt_anim_embedded_normal", animEmbedNames, ANIM_COUNT);

    LaiueContentCatalogDestroy(catalog);
    PlatformFree(paths);
    WriteText("texture build benchmark done\n");
    LAIUE_TEST_SUCCESS();
}
