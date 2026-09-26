// ROUND2 24-shaders: ручной A/B стенд shader_pack.c.
//
// В ALL не входит и в CTest не регистрируется: его осознанно запускает
// A/B-скрипт и читает вывод. Стенд покрывает discovery/load/validation:
// перечисление паков и загрузку активного набора (манифест + стадии) во
// всех значимых состояниях: полный, BOM, неполный, пустой, битый манифест,
// стадия-каталог.
//
// Набор данных детерминирован и создаётся в каталоге из переменной окружения
// LAIUE_R2_24_SHADERS_ROOT (fallback — рядом с исполняемым файлом). Один и тот
// же набор переиспользуют baseline и candidate, поэтому входные данные не
// меняются между версиями. Переменная LAIUE_R2_24_SHADERS_ONLY ограничивает
// прогон одним сценарием (для внешнего замера памяти).
//
// Каждый сценарий печатает свою лучшую и суммарную длительность, а также
// hash/count/status — по ним A/B-скрипт проверяет, что объём работы одинаков.

#include "content/content_catalog.h"
#include "platform/system.h"
#include "render/shader_pack.h"
#include "test_runtime.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define R2_ROOT_ENV "LAIUE_R2_24_SHADERS_ROOT"
#define R2_ONLY_ENV "LAIUE_R2_24_SHADERS_ONLY"
#define R2_ROOT_FALLBACK L"r2_24_shaders_bench_data"
#define R2_PATH_CAPACITY 256u
#define R2_STAGE_BYTES 256u

// Release LTO иначе складывает кадры всех функций в точку входа, а no-CRT
// сборке не нужен __chkstk.
#if defined(_MSC_VER) || defined(__clang__)
#define R2_NOINLINE __declspec(noinline)
#else
#define R2_NOINLINE __attribute__((noinline))
#endif

typedef enum R2ScenarioKind
{
    R2_KIND_ENUM = 0,
    R2_KIND_LOADSET,
} R2ScenarioKind;

typedef enum R2ManifestKind
{
    R2_MANIFEST_VALID = 0,
    R2_MANIFEST_BOM,
    R2_MANIFEST_INVALID,
} R2ManifestKind;

typedef struct R2Scenario
{
    const char *name;
    const wchar_t *wname;
    uint32_t kind;
    uint32_t entries;
    uint32_t manifest;
    uint32_t presentMask;
    int32_t badSlot;
    uint32_t warmup;
    uint32_t iterations;
} R2Scenario;

// presentMask: бит i — стадия i существует. badSlot >= 0 — эта стадия
// создаётся каталогом (SHADER_FILE_INVALID).
static const R2Scenario g_scenarios[] = {
    {"enum_small", L"enum_small", R2_KIND_ENUM, 128u, 0u, 0u, -1, 200u, 1500u},
    {"enum_large", L"enum_large", R2_KIND_ENUM, 2048u, 0u, 0u, -1, 10u, 40u},
    {"load_full", L"load_full", R2_KIND_LOADSET, 0u, R2_MANIFEST_VALID,
     (1u << LAIUE_SHADER_SLOT_COUNT) - 1u, -1, 100u, 1500u},
    {"load_bom", L"load_bom", R2_KIND_LOADSET, 0u, R2_MANIFEST_BOM,
     (1u << LAIUE_SHADER_SLOT_COUNT) - 1u, -1, 100u, 1500u},
    {"load_partial", L"load_partial", R2_KIND_LOADSET, 0u, R2_MANIFEST_VALID,
     (1u << 0) | (1u << 3), -1, 100u, 1500u},
    {"load_empty", L"load_empty", R2_KIND_LOADSET, 0u, R2_MANIFEST_VALID, 0u, -1,
     100u, 1500u},
    {"load_invalid", L"load_invalid", R2_KIND_LOADSET, 0u, R2_MANIFEST_INVALID,
     (1u << LAIUE_SHADER_SLOT_COUNT) - 1u, -1, 100u, 2000u},
    {"load_bad_shader", L"load_bad_shader", R2_KIND_LOADSET, 0u, R2_MANIFEST_VALID,
     (1u << LAIUE_SHADER_SLOT_COUNT) - 1u, 1, 100u, 1500u},
};
#define R2_SCENARIO_COUNT (sizeof(g_scenarios) / sizeof(g_scenarios[0]))

static const wchar_t *const g_stageNames[LAIUE_SHADER_SLOT_COUNT] = {
    L"chunk_vs.ls",    L"chunk_ps.ls", L"panorama_vs.ls",
    L"panorama_ps.ls", L"ui_vs.ls",    L"ui_ps.ls",
};

static volatile uint64_t g_benchSink;

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

static void WriteFixed(double value)
{
    if (!(value > 0.0))
    {
        WriteText("0.000");
        return;
    }
    if (value > 1000000000.0)
    {
        value = 1000000000.0;
    }
    uint64_t whole = (uint64_t)value;
    WriteUnsigned(whole);
    WriteText(".");
    uint64_t fraction = (uint64_t)((value - (double)whole) * 1000.0);
    if (fraction > 999u)
    {
        fraction = 999u;
    }
    if (fraction < 100u)
    {
        WriteText("0");
    }
    if (fraction < 10u)
    {
        WriteText("0");
    }
    WriteUnsigned(fraction);
}

static void Fail(const char *message)
{
    WriteText("r2_24_shaders benchmark failed: ");
    WriteText(message);
    WriteText("\n");
    LaiueTestRuntimeExit(1);
}

static void CopyText(wchar_t *destination, uint32_t capacity, const wchar_t *source)
{
    uint32_t index = 0u;
    while (source[index] != L'\0' && index + 1u < capacity)
    {
        destination[index] = source[index];
        ++index;
    }
    destination[index] = L'\0';
}

static bool AppendPath(wchar_t *destination, uint32_t capacity, const wchar_t *segment)
{
    uint32_t length = 0u;
    while (destination[length] != L'\0')
    {
        ++length;
    }
    if (length > 0u && destination[length - 1u] != L'/' && destination[length - 1u] != L'\\')
    {
        if (length + 1u >= capacity)
        {
            return false;
        }
        destination[length++] = L'/';
    }
    for (uint32_t index = 0u; segment[index] != L'\0'; ++index)
    {
        if (length + 1u >= capacity)
        {
            return false;
        }
        destination[length++] = segment[index];
    }
    destination[length] = L'\0';
    return true;
}

static void FormatIndex(wchar_t *output, uint32_t value)
{
    for (uint32_t digit = 0u; digit < 4u; ++digit)
    {
        output[3u - digit] = (wchar_t)(L'0' + (wchar_t)(value % 10u));
        value /= 10u;
    }
    output[4] = L'\0';
}

static uint64_t HashByte(uint64_t hash, uint8_t value)
{
    hash ^= (uint64_t)value;
    hash *= 1099511628211ULL;
    return hash;
}

static uint64_t HashU32(uint64_t hash, uint32_t value)
{
    for (uint32_t index = 0u; index < 4u; ++index)
    {
        hash = HashByte(hash, (uint8_t)(value >> (index * 8u)));
    }
    return hash;
}

static R2_NOINLINE bool ReadBenchRoot(wchar_t *output, uint32_t capacity)
{
    char utf8[LAIUE_PLATFORM_PATH_CAPACITY];
    uint32_t length = PlatformGetEnvironmentUtf8(R2_ROOT_ENV, utf8, sizeof(utf8));
    if (length > 0u)
    {
        return PlatformUtf8ToWide(utf8, length, output, capacity, NULL);
    }
    if (!PlatformExecutableDirectory(output, capacity))
    {
        return false;
    }
    return AppendPath(output, capacity, R2_ROOT_FALLBACK);
}

static bool ReadOnlyScenario(char *output, uint32_t capacity)
{
    output[0] = '\0';
    char utf8[128];
    uint32_t length = PlatformGetEnvironmentUtf8(R2_ONLY_ENV, utf8, sizeof(utf8));
    if (length == 0u || length >= capacity)
    {
        return false;
    }
    for (uint32_t index = 0u; index < length; ++index)
    {
        output[index] = utf8[index];
    }
    output[length] = '\0';
    return true;
}

static bool ScenarioSelected(const char *only, const char *name)
{
    if (only[0] == '\0' || (only[0] == 'a' && only[1] == 'l' && only[2] == 'l' &&
                            only[3] == '\0'))
    {
        return true;
    }
    uint32_t index = 0u;
    while (only[index] != '\0' && name[index] != '\0' && only[index] == name[index])
    {
        ++index;
    }
    return only[index] == '\0' && name[index] == '\0';
}

// Создаёт <root>/<scenario>/shaders/...; повторный вызов при готовом маркере
// ничего не пишет.
static R2_NOINLINE bool EnsureEnumDataset(const wchar_t *root, const wchar_t *scenarioName,
                                          uint32_t count)
{
    wchar_t scenarioDir[R2_PATH_CAPACITY];
    wchar_t shadersDir[R2_PATH_CAPACITY];
    wchar_t marker[R2_PATH_CAPACITY];
    CopyText(scenarioDir, R2_PATH_CAPACITY, root);
    if (!AppendPath(scenarioDir, R2_PATH_CAPACITY, scenarioName) ||
        !PlatformCreateDirectory(scenarioDir))
    {
        return false;
    }
    CopyText(shadersDir, R2_PATH_CAPACITY, scenarioDir);
    if (!AppendPath(shadersDir, R2_PATH_CAPACITY, L"shaders") ||
        !PlatformCreateDirectory(shadersDir))
    {
        return false;
    }
    CopyText(marker, R2_PATH_CAPACITY, scenarioDir);
    if (!AppendPath(marker, R2_PATH_CAPACITY, L"prepared.txt"))
    {
        return false;
    }
    if (PlatformPathExists(marker))
    {
        return true;
    }

    for (uint32_t index = 0u; index < count; ++index)
    {
        wchar_t name[16];
        name[0] = L'p';
        name[1] = L'k';
        FormatIndex(&name[2], index);
        name[6] = L'.';
        name[7] = L'l';
        name[8] = L's';
        name[9] = L'p';
        name[10] = L'\0';

        wchar_t path[R2_PATH_CAPACITY];
        CopyText(path, R2_PATH_CAPACITY, shadersDir);
        if (!AppendPath(path, R2_PATH_CAPACITY, name) || !PlatformCreateDirectory(path))
        {
            return false;
        }
    }

    static const uint8_t oneByte[1] = {0u};
    return PlatformWriteEntireFile(marker, oneByte, sizeof(oneByte));
}

static R2_NOINLINE bool WriteManifest(const wchar_t *packDir, uint32_t kind)
{
    static const char valid[] = "LAIUE SHADER 1\n"
                                "name = Bench\n"
                                "contract = 1\n";
    static const char invalid[] = "LAIUE SHADER 1\n"
                                  "name = Bench\n"
                                  "contract = 2\n";
    wchar_t path[R2_PATH_CAPACITY];
    CopyText(path, R2_PATH_CAPACITY, packDir);
    if (!AppendPath(path, R2_PATH_CAPACITY, L"pack.lm"))
    {
        return false;
    }
    static const unsigned char bom[3] = {0xefu, 0xbbu, 0xbfu};
    char manifest[160];
    uint32_t length = 0u;
    if (kind == R2_MANIFEST_BOM)
    {
        for (uint32_t index = 0u; index < 3u; ++index)
        {
            manifest[length++] = (char)bom[index];
        }
    }
    const char *source = kind == R2_MANIFEST_INVALID ? invalid : valid;
    for (uint32_t index = 0u; source[index] != '\0'; ++index)
    {
        manifest[length++] = source[index];
    }
    return PlatformWriteEntireFile(path, manifest, length);
}

static R2_NOINLINE bool WriteStage(const wchar_t *packDir, uint32_t slot, bool asDirectory)
{
    wchar_t path[R2_PATH_CAPACITY];
    CopyText(path, R2_PATH_CAPACITY, packDir);
    if (!AppendPath(path, R2_PATH_CAPACITY, g_stageNames[slot]))
    {
        return false;
    }
    if (asDirectory)
    {
        return PlatformCreateDirectory(path);
    }
    uint8_t bytes[R2_STAGE_BYTES];
    for (uint32_t index = 0u; index < R2_STAGE_BYTES; ++index)
    {
        bytes[index] = (uint8_t)(slot * 31u + index * 7u + 3u);
    }
    return PlatformWriteEntireFile(path, bytes, R2_STAGE_BYTES);
}

static R2_NOINLINE bool EnsureLoadDataset(const wchar_t *root, const R2Scenario *scenario)
{
    wchar_t scenarioDir[R2_PATH_CAPACITY];
    wchar_t shadersDir[R2_PATH_CAPACITY];
    wchar_t packDir[R2_PATH_CAPACITY];
    wchar_t marker[R2_PATH_CAPACITY];
    CopyText(scenarioDir, R2_PATH_CAPACITY, root);
    if (!AppendPath(scenarioDir, R2_PATH_CAPACITY, scenario->wname) ||
        !PlatformCreateDirectory(scenarioDir))
    {
        return false;
    }
    CopyText(shadersDir, R2_PATH_CAPACITY, scenarioDir);
    if (!AppendPath(shadersDir, R2_PATH_CAPACITY, L"shaders") ||
        !PlatformCreateDirectory(shadersDir))
    {
        return false;
    }
    CopyText(packDir, R2_PATH_CAPACITY, shadersDir);
    if (!AppendPath(packDir, R2_PATH_CAPACITY, L"Active.lsp") ||
        !PlatformCreateDirectory(packDir))
    {
        return false;
    }
    CopyText(marker, R2_PATH_CAPACITY, scenarioDir);
    if (!AppendPath(marker, R2_PATH_CAPACITY, L"prepared.txt"))
    {
        return false;
    }
    if (PlatformPathExists(marker))
    {
        return true;
    }

    if (!WriteManifest(packDir, scenario->manifest))
    {
        return false;
    }
    for (uint32_t slot = 0u; slot < (uint32_t)LAIUE_SHADER_SLOT_COUNT; ++slot)
    {
        if ((scenario->presentMask & (1u << slot)) == 0u)
        {
            continue;
        }
        if (!WriteStage(packDir, slot, scenario->badSlot == (int32_t)slot))
        {
            return false;
        }
    }

    static const char active[] = "Active.lsp\n";
    wchar_t activePath[R2_PATH_CAPACITY];
    CopyText(activePath, R2_PATH_CAPACITY, shadersDir);
    if (!AppendPath(activePath, R2_PATH_CAPACITY, L"active.txt") ||
        !PlatformWriteEntireFile(activePath, active, sizeof(active) - 1u))
    {
        return false;
    }

    static const uint8_t oneByte[1] = {0u};
    return PlatformWriteEntireFile(marker, oneByte, sizeof(oneByte));
}

typedef struct R2Result
{
    double best;
    double sum;
    uint64_t hash;
    uint32_t count;
    uint32_t status;
} R2Result;

static uint64_t HashList(const ShaderPackList *list)
{
    uint64_t hash = 1469598103934665603ULL;
    hash = HashU32(hash, list->count);
    for (uint32_t index = 0u; index < list->count; ++index)
    {
        const wchar_t *character = list->entries[index].name;
        while (*character != L'\0')
        {
            hash = HashByte(hash, (uint8_t)(uint16_t)(*character));
            ++character;
        }
        hash = HashByte(hash, list->entries[index].active ? 1u : 0u);
        hash = HashByte(hash, 0x1fu);
    }
    return hash;
}

static uint32_t PopCount(uint32_t value)
{
    uint32_t count = 0u;
    while (value != 0u)
    {
        count += value & 1u;
        value >>= 1u;
    }
    return count;
}

static uint64_t HashSet(uint32_t status, const LaiueShaderSet *set)
{
    uint64_t hash = HashU32(1469598103934665603ULL, status);
    if (set == NULL)
    {
        return hash;
    }
    hash = HashU32(hash, set->overrideMask);
    for (uint32_t slot = 0u; slot < (uint32_t)LAIUE_SHADER_SLOT_COUNT; ++slot)
    {
        if ((set->overrideMask & (1u << slot)) == 0u)
        {
            continue;
        }
        hash = HashU32(hash, slot);
        hash = HashU32(hash, set->bytecode[slot].sizeBytes);
        const uint8_t *bytes = (const uint8_t *)set->bytecode[slot].bytes;
        uint32_t sample = set->bytecode[slot].sizeBytes < 4u ? set->bytecode[slot].sizeBytes : 4u;
        for (uint32_t index = 0u; index < sample; ++index)
        {
            hash = HashByte(hash, bytes[index]);
        }
    }
    return hash;
}

static R2_NOINLINE void MeasureEnum(const wchar_t *scenarioDir, uint32_t expectedCount,
                                    uint32_t warmup, uint32_t iterations, R2Result *out)
{
    LaiueContentCatalog *catalog = LaiueContentCatalogCreate(scenarioDir);
    if (catalog == NULL)
    {
        Fail("catalog creation");
    }

    for (uint32_t index = 0u; index < warmup; ++index)
    {
        ShaderPackList list;
        if (!ShaderPackEnumerateFrom(catalog, &list) || list.count != expectedCount)
        {
            Fail("warmup enumeration");
        }
        ShaderPackListRelease(&list);
    }

    double best = 0.0;
    double sum = 0.0;
    uint64_t hash = 0u;
    for (uint32_t index = 0u; index < iterations; ++index)
    {
        double begin = PlatformMonotonicSeconds();
        ShaderPackList list;
        if (!ShaderPackEnumerateFrom(catalog, &list))
        {
            Fail("enumeration");
        }
        double elapsed = PlatformMonotonicSeconds() - begin;
        if (list.count != expectedCount)
        {
            Fail("enumeration count");
        }
        hash = HashList(&list);
        ShaderPackListRelease(&list);
        if (index == 0u || elapsed < best)
        {
            best = elapsed;
        }
        sum += elapsed;
    }
    LaiueContentCatalogDestroy(catalog);
    g_benchSink += hash;
    out->best = best;
    out->sum = sum;
    out->hash = hash;
    out->count = expectedCount;
    out->status = 0u;
}

static R2_NOINLINE void MeasureLoadSet(const wchar_t *scenarioDir, uint32_t warmup,
                                       uint32_t iterations, R2Result *out)
{
    LaiueContentCatalog *catalog = LaiueContentCatalogCreate(scenarioDir);
    if (catalog == NULL)
    {
        Fail("catalog creation");
    }

    for (uint32_t index = 0u; index < warmup; ++index)
    {
        ShaderPackLoadedSet *set = ShaderPackLoadActiveSet(catalog, NULL);
        ShaderPackLoadedSetRelease(set);
    }

    double best = 0.0;
    double sum = 0.0;
    uint64_t hash = 0u;
    uint32_t status = 0u;
    uint32_t overrides = 0u;
    for (uint32_t index = 0u; index < iterations; ++index)
    {
        double begin = PlatformMonotonicSeconds();
        ShaderPackLoadStatus loadStatus = SHADER_PACK_LOAD_NOT_ATTEMPTED;
        ShaderPackLoadedSet *set = ShaderPackLoadActiveSet(catalog, &loadStatus);
        double elapsed = PlatformMonotonicSeconds() - begin;
        const LaiueShaderSet *view = ShaderPackLoadedSetGet(set);
        hash = HashSet((uint32_t)loadStatus, view);
        status = (uint32_t)loadStatus;
        overrides = view != NULL ? PopCount(view->overrideMask) : 0u;
        ShaderPackLoadedSetRelease(set);
        if (index == 0u || elapsed < best)
        {
            best = elapsed;
        }
        sum += elapsed;
    }
    LaiueContentCatalogDestroy(catalog);
    g_benchSink += hash;
    out->best = best;
    out->sum = sum;
    out->hash = hash;
    out->count = overrides;
    out->status = status;
}

LAIUE_TEST_ENTRY(R2ShadersBenchmarkEntryPoint)
{
    wchar_t root[R2_PATH_CAPACITY];
    if (!ReadBenchRoot(root, R2_PATH_CAPACITY))
    {
        Fail("benchmark root");
    }
    char only[64];
    ReadOnlyScenario(only, sizeof(only));

    for (uint32_t index = 0u; index < R2_SCENARIO_COUNT; ++index)
    {
        const R2Scenario *scenario = &g_scenarios[index];
        if (!ScenarioSelected(only, scenario->name))
        {
            continue;
        }
        if (!PlatformCreateDirectory(root))
        {
            Fail("root creation");
        }

        wchar_t scenarioDir[R2_PATH_CAPACITY];
        CopyText(scenarioDir, R2_PATH_CAPACITY, root);
        if (!AppendPath(scenarioDir, R2_PATH_CAPACITY, scenario->wname))
        {
            Fail("scenario path");
        }

        R2Result result;
        result.best = 0.0;
        result.sum = 0.0;
        result.hash = 0u;
        result.count = 0u;
        result.status = 0u;
        if (scenario->kind == R2_KIND_ENUM)
        {
            if (!EnsureEnumDataset(root, scenario->wname, scenario->entries))
            {
                Fail("enum dataset");
            }
            MeasureEnum(scenarioDir, scenario->entries + 1u, scenario->warmup,
                        scenario->iterations, &result);
        }
        else
        {
            if (!EnsureLoadDataset(root, scenario))
            {
                Fail("load dataset");
            }
            // Активный пак живёт в <scenario>/shaders, каталог содержимого —
            // сам <scenario>.
            MeasureLoadSet(scenarioDir, scenario->warmup, scenario->iterations, &result);
        }

        WriteText("bench scenario=");
        WriteText(scenario->name);
        WriteText(" iterations=");
        WriteUnsigned(scenario->iterations);
        WriteText(" warmup=");
        WriteUnsigned(scenario->warmup);
        WriteText(" best_us=");
        WriteFixed(result.best * 1000000.0);
        WriteText(" sum_us=");
        WriteFixed(result.sum * 1000000.0);
        WriteText(" hash=");
        WriteUnsigned(result.hash);
        WriteText(" count=");
        WriteUnsigned(result.count);
        WriteText(" status=");
        WriteUnsigned(result.status);
        WriteText("\n");
    }

    WriteText("r2_24_shaders benchmark: done sink=");
    WriteUnsigned(g_benchSink != 0u ? 1u : 0u);
    WriteText("\n");
    LAIUE_TEST_SUCCESS();
}
