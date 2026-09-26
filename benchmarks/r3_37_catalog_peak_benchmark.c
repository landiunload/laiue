// ROUND 3 harness для задачи 37-catalog-peak (снижение transient peak памяти
// однопроходного LaiueContentCatalogEnumerate без второго обхода каталога).
//
// Один и тот же exe линкуется с laiue_content.dll; baseline и candidate
// различаются только подменённой DLL, поэтому вход, операции и порядок вызовов
// идентичны по построению.
//
// Сценарии детерминированно строятся в каталоге из LAIUE_R3_37_ROOT (fallback —
// рядом с exe). LAIUE_R3_37_SCALE умножает число измеряемых итераций.
// LAIUE_R3_37_ONLY=<scenario> оставляет ровно один сценарий: тогда измеренный
// peak процесса относится только к нему. LAIUE_R3_37_MODE=retained выполняет
// ровно одно перечисление и печатает удержанную память (commit до/при
// удержанном списке/после release) вместо таймингов.
//
// Harness проверяет count и строгий порядок каждого результата и падает
// (exit 1), если результат разошёлся, — иначе A/B сравнивал бы разные
// workloads.

#include "content/content_catalog.h"
#include "platform/system.h"
#include "test_runtime.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define R3_ROOT_ENV "LAIUE_R3_37_ROOT"
#define R3_SCALE_ENV "LAIUE_R3_37_SCALE"
#define R3_ONLY_ENV "LAIUE_R3_37_ONLY"
#define R3_MODE_ENV "LAIUE_R3_37_MODE"
#define R3_ROOT_FALLBACK L"r3_37_catalog_data"
// Пути harness короткие; меньшая ёмкость не даёт LTO сложить кадры всех
// функций в точку входа и потянуть __chkstk, которого в no-CRT сборке нет.
#define R3_PATH_CAPACITY 512u
#define R3_NAME_CAPACITY 160u
// 113 'n' + 4 цифры + ".ls" = 120 символов: длинное, но валидное имя.
#define R3_LONG_PREFIX_LENGTH 113u
#define R3_LONG_TOTAL_NAME 120u

#if defined(_MSC_VER) || defined(__clang__)
#define R3_NOINLINE __declspec(noinline)
#else
#define R3_NOINLINE __attribute__((noinline))
#endif

typedef enum R3Kind
{
    R3_KIND_FILES = 0, // N файлов primary-расширения
    R3_KIND_DIRS,      // N каталогов (пак-формат), + active.txt
    R3_KIND_MIXED,     // N primary + extra несовпадающих файлов
    R3_KIND_LONG,      // N файлов с длинными (120 символов) именами
    R3_KIND_EMPTY,     // пустой каталог категории
} R3Kind;

typedef struct R3Scenario
{
    const wchar_t *name;
    const wchar_t *category;
    LaiueContentType type;
    R3Kind kind;
    uint32_t count;
    uint32_t extra;
    uint32_t warmup;
    uint32_t iterations;
    bool expectFailure;
    bool timed;
} R3Scenario;

// iterations подобраны так, чтобы каждый timed-сценарий давал заметный прогон
// (десятки-сотни мс) без выхода за 2-3 минуты на блок.
static const R3Scenario g_scenarios[] = {
    {L"files_64", L"shaders", LAIUE_CONTENT_SHADER, R3_KIND_FILES, 64u, 0u, 500u, 500u, false,
     true},
    {L"files_512", L"shaders", LAIUE_CONTENT_SHADER, R3_KIND_FILES, 512u, 0u, 300u, 400u, false,
     true},
    {L"files_2048", L"shaders", LAIUE_CONTENT_SHADER, R3_KIND_FILES, 2048u, 0u, 100u, 200u, false,
     true},
    // Нестепенное N: проверяет overshoot роста и стоимость подрезки.
    {L"files_3000", L"shaders", LAIUE_CONTENT_SHADER, R3_KIND_FILES, 3000u, 0u, 80u, 150u, false,
     true},
    {L"files_4096", L"shaders", LAIUE_CONTENT_SHADER, R3_KIND_FILES, 4096u, 0u, 50u, 100u, false,
     true},
    // Длинные имена: худший случай для компактной staging-раскладки.
    {L"longnames_1024", L"shaders", LAIUE_CONTENT_SHADER, R3_KIND_LONG, 1024u, 0u, 100u, 150u,
     false, true},
    {L"mixed_2048", L"shaders", LAIUE_CONTENT_SHADER, R3_KIND_MIXED, 2048u, 2048u, 60u, 120u, false,
     true},
    {L"packs_512", L"shaders", LAIUE_CONTENT_SHADER_PACK, R3_KIND_DIRS, 512u, 0u, 300u, 400u,
     false, true},
    {L"empty", L"shaders", LAIUE_CONTENT_SHADER, R3_KIND_EMPTY, 0u, 0u, 5000u, 10000u, false, true},
    {L"over_limit", L"shaders", LAIUE_CONTENT_SHADER, R3_KIND_FILES, 4097u, 0u, 0u, 1u, true,
     false},
};
#define R3_SCENARIO_COUNT (sizeof(g_scenarios) / sizeof(g_scenarios[0]))

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
    if (value > 1000000000000.0)
    {
        value = 1000000000000.0;
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

static void WriteScenarioName(const wchar_t *name)
{
    for (const wchar_t *character = name; *character != L'\0'; ++character)
    {
        char ascii[2];
        ascii[0] = (char)(*character & 0x7f);
        ascii[1] = '\0';
        WriteText(ascii);
    }
}

static void Fail(const char *message)
{
    WriteText("r3_37_catalog benchmark failed: ");
    WriteText(message);
    WriteText("\n");
    LaiueTestRuntimeExit(1);
}

// --- process memory (Windows only, no CRT) ---
#if defined(_WIN32)
typedef struct R3ProcessMemoryCounters
{
    uint32_t cb;
    uint32_t pageFaultCount;
    uint64_t peakWorkingSetSize;
    uint64_t workingSetSize;
    uint64_t quotaPeakPagedPoolUsage;
    uint64_t quotaPagedPoolUsage;
    uint64_t quotaPeakNonPagedPoolUsage;
    uint64_t quotaNonPagedPoolUsage;
    uint64_t pagefileUsage;
    uint64_t peakPagefileUsage;
} R3ProcessMemoryCounters;

__declspec(dllimport) void *__stdcall GetCurrentProcess(void);
__declspec(dllimport) int __stdcall GetProcessMemoryInfo(void *process,
                                                       R3ProcessMemoryCounters *counters,
                                                       uint32_t size);

static bool SampleMemory(R3ProcessMemoryCounters *out)
{
    for (uint32_t index = 0; index < sizeof(*out); ++index)
    {
        ((uint8_t *)out)[index] = 0u;
    }
    out->cb = (uint32_t)sizeof(*out);
    return GetProcessMemoryInfo(GetCurrentProcess(), out, (uint32_t)sizeof(*out)) != 0;
}

static void ReportPeakMemory(void)
{
    R3ProcessMemoryCounters counters;
    if (SampleMemory(&counters))
    {
        WriteText("peak_commit_bytes=");
        WriteUnsigned(counters.peakPagefileUsage);
        WriteText(" peak_working_set_bytes=");
        WriteUnsigned(counters.peakWorkingSetSize);
        WriteText("\n");
    }
    else
    {
        WriteText("peak_commit_bytes=unknown peak_working_set_bytes=unknown\n");
    }
}
#else
static void ReportPeakMemory(void)
{
    WriteText("peak_commit_bytes=unknown peak_working_set_bytes=unknown\n");
}
#endif

// --- path/name helpers ---
static R3_NOINLINE void CopyText(wchar_t *destination, uint32_t capacity, const wchar_t *source)
{
    uint32_t index = 0u;
    while (source[index] != L'\0' && index + 1u < capacity)
    {
        destination[index] = source[index];
        ++index;
    }
    destination[index] = L'\0';
}

static R3_NOINLINE bool AppendPath(wchar_t *destination, uint32_t capacity, const wchar_t *segment)
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

// prefix + 4 цифры + suffix.
static R3_NOINLINE bool FormatName(wchar_t *output, const wchar_t *prefix, uint32_t value,
                                   const wchar_t *suffix)
{
    uint32_t length = 0u;
    for (const wchar_t *part = prefix; *part != L'\0'; ++part)
    {
        output[length++] = *part;
    }
    for (uint32_t digit = 0u; digit < 4u; ++digit)
    {
        output[length + (3u - digit)] = (wchar_t)(L'0' + (wchar_t)(value % 10u));
        value /= 10u;
    }
    length += 4u;
    for (const wchar_t *part = suffix; *part != L'\0'; ++part)
    {
        output[length++] = *part;
    }
    output[length] = L'\0';
    return true;
}

// 113 'n' + 4 цифры + ".ls" = 120 символов.
static R3_NOINLINE bool FormatLongName(wchar_t *output, uint32_t value)
{
    uint32_t length = 0u;
    for (uint32_t index = 0u; index < R3_LONG_PREFIX_LENGTH; ++index)
    {
        output[length++] = L'n';
    }
    for (uint32_t digit = 0u; digit < 4u; ++digit)
    {
        output[length + (3u - digit)] = (wchar_t)(L'0' + (wchar_t)(value % 10u));
        value /= 10u;
    }
    length += 4u;
    output[length++] = L'.';
    output[length++] = L'l';
    output[length++] = L's';
    output[length] = L'\0';
    return length == R3_LONG_TOTAL_NAME;
}

static R3_NOINLINE bool WriteMarker(const wchar_t *scenarioDir)
{
    wchar_t marker[R3_PATH_CAPACITY];
    CopyText(marker, R3_PATH_CAPACITY, scenarioDir);
    if (!AppendPath(marker, R3_PATH_CAPACITY, L"prepared.txt"))
    {
        return false;
    }
    static const uint8_t oneByte[1] = {0u};
    return PlatformWriteEntireFile(marker, oneByte, sizeof(oneByte));
}

static R3_NOINLINE bool CreateFiles(const wchar_t *contentDir, const wchar_t *prefix,
                                    const wchar_t *suffix, uint32_t count)
{
    static const uint8_t oneByte[1] = {0u};
    for (uint32_t index = 0u; index < count; ++index)
    {
        wchar_t name[R3_NAME_CAPACITY];
        FormatName(name, prefix, index, suffix);
        wchar_t path[R3_PATH_CAPACITY];
        CopyText(path, R3_PATH_CAPACITY, contentDir);
        if (!AppendPath(path, R3_PATH_CAPACITY, name) ||
            !PlatformWriteEntireFile(path, oneByte, sizeof(oneByte)))
        {
            return false;
        }
    }
    return true;
}

static R3_NOINLINE bool CreateLongFiles(const wchar_t *contentDir, uint32_t count)
{
    static const uint8_t oneByte[1] = {0u};
    for (uint32_t index = 0u; index < count; ++index)
    {
        wchar_t name[R3_NAME_CAPACITY];
        if (!FormatLongName(name, index))
        {
            return false;
        }
        wchar_t path[R3_PATH_CAPACITY];
        CopyText(path, R3_PATH_CAPACITY, contentDir);
        if (!AppendPath(path, R3_PATH_CAPACITY, name) ||
            !PlatformWriteEntireFile(path, oneByte, sizeof(oneByte)))
        {
            return false;
        }
    }
    return true;
}

static R3_NOINLINE bool CreateDirs(const wchar_t *contentDir, const wchar_t *prefix,
                                   const wchar_t *suffix, uint32_t count)
{
    for (uint32_t index = 0u; index < count; ++index)
    {
        wchar_t name[R3_NAME_CAPACITY];
        FormatName(name, prefix, index, suffix);
        wchar_t path[R3_PATH_CAPACITY];
        CopyText(path, R3_PATH_CAPACITY, contentDir);
        if (!AppendPath(path, R3_PATH_CAPACITY, name) || !PlatformCreateDirectory(path))
        {
            return false;
        }
    }
    return true;
}

static R3_NOINLINE bool WriteActiveFile(const wchar_t *contentDir, uint32_t index)
{
    wchar_t name[R3_NAME_CAPACITY];
    FormatName(name, L"pack_", index, L".lsp");
    char utf8[64];
    uint32_t byteCount = 0u;
    if (!PlatformWideToUtf8(name, utf8, sizeof(utf8) - 2u, &byteCount))
    {
        return false;
    }
    utf8[byteCount++] = '\n';
    wchar_t path[R3_PATH_CAPACITY];
    CopyText(path, R3_PATH_CAPACITY, contentDir);
    if (!AppendPath(path, R3_PATH_CAPACITY, L"active.txt"))
    {
        return false;
    }
    return PlatformWriteEntireFile(path, utf8, byteCount);
}

static R3_NOINLINE bool EnsureDataset(const wchar_t *root, const R3Scenario *scenario,
                                      wchar_t *outScenarioDir)
{
    if (!PlatformCreateDirectory(root))
    {
        return false;
    }
    CopyText(outScenarioDir, R3_PATH_CAPACITY, root);
    if (!AppendPath(outScenarioDir, R3_PATH_CAPACITY, scenario->name) ||
        !PlatformCreateDirectory(outScenarioDir))
    {
        return false;
    }

    wchar_t contentDir[R3_PATH_CAPACITY];
    CopyText(contentDir, R3_PATH_CAPACITY, outScenarioDir);
    if (!AppendPath(contentDir, R3_PATH_CAPACITY, scenario->category) ||
        !PlatformCreateDirectory(contentDir))
    {
        return false;
    }

    wchar_t marker[R3_PATH_CAPACITY];
    CopyText(marker, R3_PATH_CAPACITY, outScenarioDir);
    if (!AppendPath(marker, R3_PATH_CAPACITY, L"prepared.txt") || PlatformPathExists(marker))
    {
        return PlatformPathExists(marker);
    }

    bool created = true;
    switch (scenario->kind)
    {
    case R3_KIND_FILES:
        created = CreateFiles(contentDir, L"shader_", L".ls", scenario->count);
        break;
    case R3_KIND_DIRS:
        created = CreateDirs(contentDir, L"pack_", L".lsp", scenario->count) &&
                  WriteActiveFile(contentDir, scenario->count / 2u);
        break;
    case R3_KIND_MIXED:
        created = CreateFiles(contentDir, L"shader_", L".ls", scenario->count) &&
                  CreateFiles(contentDir, L"noise_", L".txt", scenario->extra);
        break;
    case R3_KIND_LONG:
        created = CreateLongFiles(contentDir, scenario->count);
        break;
    case R3_KIND_EMPTY:
        created = true;
        break;
    }
    if (!created)
    {
        return false;
    }
    return WriteMarker(outScenarioDir);
}

static int32_t NameCompare(const wchar_t *left, const wchar_t *right)
{
    uint32_t index = 0u;
    while (left[index] != L'\0' && right[index] != L'\0' && left[index] == right[index])
    {
        ++index;
    }
    return left[index] < right[index] ? -1 : left[index] > right[index] ? 1 : 0;
}

// Строгий возрастающий порядок и отсутствие повторов.
static bool ListIsStrictlySorted(const LaiueContentList *list)
{
    for (uint32_t index = 1u; index < list->count; ++index)
    {
        if (NameCompare(list->entries[index - 1u].name, list->entries[index].name) >= 0)
        {
            return false;
        }
    }
    return true;
}

static uint64_t HashList(const LaiueContentList *list)
{
    uint64_t hash = 1469598103934665603ULL;
    for (uint32_t index = 0u; index < list->count; ++index)
    {
        const wchar_t *character = list->entries[index].name;
        while (*character != L'\0')
        {
            hash ^= (uint64_t)(uint16_t)(*character);
            hash *= 1099511628211ULL;
            ++character;
        }
        hash ^= list->entries[index].active ? 0x11U : 0x22U;
        hash *= 1099511628211ULL;
        hash ^= list->entries[index].directory ? 0x33U : 0x44U;
        hash *= 1099511628211ULL;
    }
    return hash;
}

static void VerifyResult(const R3Scenario *scenario, bool ok, const LaiueContentList *list)
{
    if (scenario->expectFailure)
    {
        if (ok)
        {
            Fail("expected enumeration failure was not observed");
        }
        return;
    }
    if (!ok)
    {
        Fail("unexpected enumeration failure");
    }
    if (list->count != scenario->count)
    {
        Fail("unexpected entry count");
    }
    if (list->count > 0u && list->entries == NULL)
    {
        Fail("non-zero count without entries");
    }
    if (!ListIsStrictlySorted(list))
    {
        Fail("enumeration result is not strictly sorted");
    }
}

static R3_NOINLINE double RunScenarioTimed(LaiueContentCatalog *catalog,
                                           const R3Scenario *scenario, uint32_t iterations,
                                           uint64_t *outHash)
{
    for (uint32_t index = 0u; index < scenario->warmup; ++index)
    {
        LaiueContentList list;
        bool ok = LaiueContentCatalogEnumerate(catalog, scenario->type, &list);
        VerifyResult(scenario, ok, &list);
        if (ok)
        {
            LaiueContentListRelease(&list);
        }
    }

    double total = 0.0;
    uint64_t hash = 0u;
    for (uint32_t index = 0u; index < iterations; ++index)
    {
        double begin = PlatformMonotonicSeconds();
        LaiueContentList list;
        bool ok = LaiueContentCatalogEnumerate(catalog, scenario->type, &list);
        double elapsed = PlatformMonotonicSeconds() - begin;
        VerifyResult(scenario, ok, &list);
        hash = HashList(&list);
        if (ok)
        {
            LaiueContentListRelease(&list);
        }
        total += elapsed;
    }
    g_benchSink += hash;
    *outHash = hash;
    return total;
}

static void RunScenarioOnce(LaiueContentCatalog *catalog, const R3Scenario *scenario)
{
    LaiueContentList list;
    bool ok = LaiueContentCatalogEnumerate(catalog, scenario->type, &list);
    VerifyResult(scenario, ok, &list);
    if (ok)
    {
        LaiueContentListRelease(&list);
    }
}

static uint32_t ReadScale(void)
{
    char utf8[32];
    uint32_t length = PlatformGetEnvironmentUtf8(R3_SCALE_ENV, utf8, sizeof(utf8));
    if (length == 0u)
    {
        return 1u;
    }
    uint32_t value = 0u;
    for (uint32_t index = 0u; index < length; ++index)
    {
        if (utf8[index] < '0' || utf8[index] > '9')
        {
            return value > 0u ? value : 1u;
        }
        value = value * 10u + (uint32_t)(utf8[index] - '0');
        if (value > 1000u)
        {
            return 1000u;
        }
    }
    return value > 0u ? value : 1u;
}

static bool NameEqualsAscii(const wchar_t *name, const char *ascii, uint32_t asciiLength)
{
    uint32_t index = 0u;
    while (name[index] != L'\0' && index < asciiLength)
    {
        char folded = (char)(name[index] & 0x7f);
        if (folded >= 'A' && folded <= 'Z')
        {
            folded = (char)(folded + ('a' - 'A'));
        }
        if (folded != ascii[index])
        {
            return false;
        }
        ++index;
    }
    return name[index] == L'\0' && index == asciiLength;
}

static bool ScenarioSelected(const R3Scenario *scenario, const char *only, uint32_t onlyLength)
{
    return onlyLength == 0u || NameEqualsAscii(scenario->name, only, onlyLength);
}

static bool ModeIsRetained(void)
{
    char utf8[16];
    uint32_t length = PlatformGetEnvironmentUtf8(R3_MODE_ENV, utf8, sizeof(utf8));
    return length == 8u && utf8[0] == 'r' && utf8[1] == 'e' && utf8[2] == 't' &&
           utf8[3] == 'a' && utf8[4] == 'i' && utf8[5] == 'n' && utf8[6] == 'e' &&
           utf8[7] == 'd';
}

LAIUE_TEST_ENTRY(R3CatalogPeakBenchmarkEntryPoint)
{
    wchar_t root[R3_PATH_CAPACITY];
    char utf8Root[512];
    uint32_t rootLength = PlatformGetEnvironmentUtf8(R3_ROOT_ENV, utf8Root, sizeof(utf8Root));
    if (rootLength > 0u)
    {
        if (!PlatformUtf8ToWide(utf8Root, rootLength, root, R3_PATH_CAPACITY, NULL))
        {
            Fail("benchmark root from environment");
        }
    }
    else
    {
        if (!PlatformExecutableDirectory(root, R3_PATH_CAPACITY) ||
            !AppendPath(root, R3_PATH_CAPACITY, R3_ROOT_FALLBACK))
        {
            Fail("benchmark root default");
        }
    }

    char only[64];
    uint32_t onlyLength = PlatformGetEnvironmentUtf8(R3_ONLY_ENV, only, sizeof(only));
    if (onlyLength >= sizeof(only))
    {
        onlyLength = 0u;
    }
    bool retainedMode = ModeIsRetained();

    uint32_t scale = ReadScale();
    double grandTotal = 0.0;
    uint64_t grandHash = 1469598103934665603ULL;

    for (uint32_t index = 0u; index < R3_SCENARIO_COUNT; ++index)
    {
        const R3Scenario *scenario = &g_scenarios[index];
        if (!ScenarioSelected(scenario, only, onlyLength))
        {
            continue;
        }
        wchar_t scenarioDir[R3_PATH_CAPACITY];
        if (!EnsureDataset(root, scenario, scenarioDir))
        {
            Fail("dataset creation");
        }

        LaiueContentCatalog *catalog = LaiueContentCatalogCreate(scenarioDir);
        if (catalog == NULL)
        {
            Fail("catalog creation");
        }

        if (retainedMode)
        {
#if defined(_WIN32)
            R3ProcessMemoryCounters before;
            R3ProcessMemoryCounters held;
            R3ProcessMemoryCounters after;
            bool haveBefore = SampleMemory(&before);
            (void)haveBefore;
            LaiueContentList list;
            bool ok = LaiueContentCatalogEnumerate(catalog, scenario->type, &list);
            VerifyResult(scenario, ok, &list);
            bool haveHeld = SampleMemory(&held);
            uint64_t heldCommit = haveHeld ? held.pagefileUsage : 0u;
            uint64_t heldWs = haveHeld ? held.workingSetSize : 0u;
            LaiueContentListRelease(&list);
            bool haveAfter = SampleMemory(&after);
            WriteText("r3_37_catalog retained scenario=");
            WriteScenarioName(scenario->name);
            WriteText(" entries=");
            WriteUnsigned(scenario->count);
            WriteText(" commit_before=");
            WriteUnsigned(before.pagefileUsage);
            WriteText(" commit_held=");
            WriteUnsigned(heldCommit);
            WriteText(" commit_after=");
            WriteUnsigned(haveAfter ? after.pagefileUsage : 0u);
            WriteText(" retained_commit_delta=");
            WriteUnsigned(heldCommit >= before.pagefileUsage
                              ? heldCommit - before.pagefileUsage
                              : 0u);
            WriteText(" ws_held=");
            WriteUnsigned(heldWs);
            WriteText(" entry_size=");
            WriteUnsigned((uint64_t)sizeof(LaiueContentEntry));
            WriteText("\n");
#else
            RunScenarioOnce(catalog, scenario);
            WriteText("r3_37_catalog retained scenario=");
            WriteScenarioName(scenario->name);
            WriteText(" entries=");
            WriteUnsigned(scenario->count);
            WriteText(" entry_size=");
            WriteUnsigned((uint64_t)sizeof(LaiueContentEntry));
            WriteText("\n");
#endif
            LaiueContentCatalogDestroy(catalog);
            continue;
        }

        if (!scenario->timed)
        {
            RunScenarioOnce(catalog, scenario);
            WriteText("r3_37_catalog scenario=");
            WriteScenarioName(scenario->name);
            WriteText(" entries=");
            WriteUnsigned(scenario->count);
            WriteText(" expect_failure=");
            WriteText(scenario->expectFailure ? "1" : "0");
            WriteText(" checked=1\n");
            LaiueContentCatalogDestroy(catalog);
            continue;
        }

        uint32_t iterations = scenario->iterations * scale;
        uint64_t hash = 0u;
        double total = RunScenarioTimed(catalog, scenario, iterations, &hash);
        grandTotal += total;
        grandHash ^= hash;
        grandHash *= 1099511628211ULL;

        WriteText("r3_37_catalog scenario=");
        WriteScenarioName(scenario->name);
        WriteText(" entries=");
        WriteUnsigned(scenario->count);
        WriteText(" iterations=");
        WriteUnsigned(iterations);
        WriteText(" total_us=");
        WriteFixed(total * 1000000.0);
        WriteText(" per_iter_us=");
        WriteFixed(total * 1000000.0 / (double)iterations);
        WriteText(" hash=");
        WriteUnsigned(hash);
        WriteText("\n");

        LaiueContentCatalogDestroy(catalog);
    }

    WriteText("r3_37_catalog TOTAL_US=");
    WriteFixed(grandTotal * 1000000.0);
    WriteText(" TOTAL_HASH=");
    WriteUnsigned(grandHash);
    WriteText(" sink=");
    WriteUnsigned(g_benchSink != 0u ? 1u : 0u);
    WriteText("\n");
    ReportPeakMemory();
    LAIUE_TEST_SUCCESS();
}
