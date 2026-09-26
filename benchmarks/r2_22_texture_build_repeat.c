// ROUND2 harness 22-texture-build: повторяемый A/B-стенд сборки текстурного
// пака (`TexturePackBuildFrom`).
//
// Отличие от `texture_build_benchmark.c`: каждый сценарий измеряется не одним
// сбором, а REPEAT сборами подряд, и наружу печатается суммарное время. Это
// снижает шум планировщика на коротких сборках (10-30 мс) и даёт измеряемый
// прогон заметно длиннее 100 мс. Набор данных и порядок сценариев повторяют
// существующий стенд, поэтому контрольные суммы пака обязаны совпадать с ним
// байт в байт, а baseline и candidate отличаются только правкой
// `src/render/texture_build.c`.
//
// Стенд компилирует внутренний `src/render/texture_build.c` прямо в свой
// исполняемый файл: сборщик не экспортируется из `laiue_render`, а новые
// экспорты запрещены.
//
// Windows собирает движок без CRT, поэтому вывод — через общий с тестами
// `test_runtime.h`, а не через printf.

#include "content/content_catalog.h"
#include "media/lt_encode.h"
#include "platform/system.h"
#include "render/texture_pack_internal.h"
#include "test_runtime.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define BENCH_ROOT_NAME L"r2_22_texture_build_repeat_v1"
#define BENCH_PACK_NAME L"Bench.ltp"
#define PATH_CAP LAIUE_PLATFORM_PATH_CAPACITY
#define FILE_CAP 512u

// Сколько раз повторить сбор одного сценария в измеряемом окне.
#define REPEAT 16u

#if defined(_MSC_VER) || defined(__clang__)
#define BENCH_NOINLINE __declspec(noinline)
#else
#define BENCH_NOINLINE __attribute__((noinline))
#endif

#define TEX_SIZE 256u
#define FRAME_BYTES (TEX_SIZE * TEX_SIZE * 4u)

#define STATIC_COUNT 32u
#define NORMAL_COUNT 32u
#define ANIM_COUNT 8u
#define ANIM_FRAMES 8u
#define TOTAL_COUNT (STATIC_COUNT + NORMAL_COUNT + ANIM_COUNT + ANIM_COUNT)

typedef struct BenchPaths
{
    wchar_t root[PATH_CAP];
    wchar_t textures[PATH_CAP];
    wchar_t pack[PATH_CAP];
    wchar_t main[PATH_CAP];
    wchar_t formats[PATH_CAP];
    wchar_t executable[PATH_CAP];
    wchar_t file[FILE_CAP];
    wchar_t normalName[LAIUE_CONTENT_NAME_CAPACITY];
    wchar_t names[TOTAL_COUNT][LAIUE_CONTENT_NAME_CAPACITY];
} BenchPaths;

static uint16_t g_durations[256];
static volatile uint64_t g_sink;

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

static BENCH_NOINLINE void ReportScenario(const char *label, double milliseconds, uint64_t checksum,
                                          const TexturePackData *pack)
{
    WriteText("RESULT scenario=");
    WriteText(label);
    WriteText(" ms=");
    WriteMilliseconds(milliseconds);
    WriteText(" checksum=");
    WriteHex(checksum);
    WriteText(" width=");
    WriteUnsigned(pack->width);
    WriteText(" mip=");
    WriteUnsigned(pack->mipCount);
    WriteText(" slices=");
    WriteUnsigned(pack->sliceCount);
    WriteText(" materials=");
    WriteUnsigned(pack->materialCount);
    WriteText(" pixelbytes=");
    WriteUnsigned(pack->pixelBytes);
    WriteText(" normals=");
    WriteUnsigned(pack->normalPixels != NULL ? 1u : 0u);
    WriteText("\n");
}

// Прогрев: один сбор вне статистики. Затем REPEAT сборов в измеряемом окне;
// между ними пак освобождается, значит каждый сбор делает всю работу заново.
// Контрольная сумма считается по отдельному сбору после окна.
static BENCH_NOINLINE void RunScenario(LaiueContentCatalog *catalog, const char *label,
                                       const wchar_t *const *names, uint32_t count)
{
    TexturePackData pack;
    TexturePackLoadStatus status = Build(catalog, names, count, &pack);
    if (status != TEXTURE_PACK_LOAD_OK && status != TEXTURE_PACK_LOAD_INCOMPLETE)
    {
        WriteText("ERROR scenario=");
        WriteText(label);
        WriteText(" status=");
        WriteUnsigned((uint32_t)status);
        WriteText("\n");
        return;
    }
    ReleasePack(&pack);

    double start = PlatformMonotonicSeconds();
    for (uint32_t rep = 0; rep < REPEAT; ++rep)
    {
        status = Build(catalog, names, count, &pack);
        if (status != TEXTURE_PACK_LOAD_OK && status != TEXTURE_PACK_LOAD_INCOMPLETE)
        {
            WriteText("ERROR scenario=");
            WriteText(label);
            WriteText(" status=");
            WriteUnsigned((uint32_t)status);
            WriteText("\n");
            return;
        }
        g_sink += pack.pixels[rep % pack.pixelBytes];
        ReleasePack(&pack);
    }
    double milliseconds = (PlatformMonotonicSeconds() - start) * 1000.0;

    status = Build(catalog, names, count, &pack);
    if (status != TEXTURE_PACK_LOAD_OK && status != TEXTURE_PACK_LOAD_INCOMPLETE)
    {
        WriteText("ERROR scenario=");
        WriteText(label);
        WriteText(" status=");
        WriteUnsigned((uint32_t)status);
        WriteText("\n");
        return;
    }
    uint64_t checksum = HashPack(&pack);
    ReportScenario(label, milliseconds, checksum, &pack);
    ReleasePack(&pack);
}

LAIUE_TEST_ENTRY(R2TextureBuildRepeatEntryPoint)
{
    BenchPaths *paths = PlatformAllocate(sizeof(*paths), true);
    if (paths == NULL)
    {
        WriteText("r2 texture build repeat: path allocation failed\n");
        LaiueTestRuntimeExit(1);
    }
    if (!PrepareContent(paths))
    {
        WriteText("r2 texture build repeat: content preparation failed\n");
        PlatformFree(paths);
        LaiueTestRuntimeExit(1);
    }

    LaiueContentCatalog *catalog = LaiueContentCatalogCreate(paths->root);
    if (catalog == NULL ||
        !LaiueContentCatalogSetActivePack(catalog, LAIUE_CONTENT_TEXTURE_PACK, BENCH_PACK_NAME))
    {
        WriteText("r2 texture build repeat: catalog setup failed\n");
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

    WriteText("r2 texture build repeat benchmark reps=");
    WriteUnsigned(REPEAT);
    WriteText("\n");
    RunScenario(catalog, "lt_static", staticNames, STATIC_COUNT);
    RunScenario(catalog, "lt_normals", normalNames, NORMAL_COUNT);
    RunScenario(catalog, "lt_anim_separate_normal", animNames, ANIM_COUNT);
    RunScenario(catalog, "lt_anim_embedded_normal", animEmbedNames, ANIM_COUNT);
    if (g_sink == UINT64_MAX) WriteText("");

    LaiueContentCatalogDestroy(catalog);
    PlatformFree(paths);
    WriteText("r2 texture build repeat benchmark done\n");
    LAIUE_TEST_SUCCESS();
}
