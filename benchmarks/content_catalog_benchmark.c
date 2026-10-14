// Ручной benchmark каталога содержимого. В ALL не входит и в CTest не
// регистрируется: его запускают осознанно и читают глазами.
//
// Меряется `LaiueContentCatalogEnumerate` на маленьких и крупных каталогах:
// два прохода по каталогу, хранение имён, сортировка и проверка
// ASCII case-collision. Именно эти шаги зависят от числа сущностей, а не от
// разрешения часов, поэтому сценарии различаются только количеством файлов.
//
// Набор данных создаётся детерминированно в каталоге из переменной окружения
// LAIUE_CONTENT_BENCH_ROOT (fallback — рядом с исполняемым файлом). Один и тот
// же набор переиспользуют baseline и candidate, поэтому сравнение честное:
// меняется только код, а не входные данные.
//
// Каталог `shaders` с файлами `.ls` соответствует одиночному формату:
// перечисление не читает active.txt и не зависит от наличия пака, поэтому
// замер не смешивается с файловым I/O за пределами самого каталога.

#include "content/content_catalog.h"
#include "platform/system.h"
#include "test_runtime.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BENCH_ROOT_ENV "LAIUE_CONTENT_BENCH_ROOT"
#define BENCH_ROOT_FALLBACK L"content_catalog_bench_data"
// Пути benchmark короткие; меньшая ёмкость держит кадр каждой функции под
// страницей и не тянет __chkstk в no-CRT исполняемый файл.
#define BENCH_PATH_CAPACITY 512u

typedef struct BenchScenario
{
    const wchar_t *name;
    uint32_t count;
    uint32_t warmup;
    uint32_t iterations;
} BenchScenario;

static const BenchScenario g_scenarios[] = {
    {L"small", 32u, 200u, 4000u},
    {L"medium", 512u, 20u, 400u},
    {L"large", 2048u, 5u, 40u},
    {L"huge", 4000u, 3u, 16u},
};
#define BENCH_SCENARIO_COUNT (sizeof(g_scenarios) / sizeof(g_scenarios[0]))

// Release LTO иначе складывает кадры всех функций в точку входа, и суммарная
// ёмкость буферов путей тянет __chkstk, которого в no-CRT сборке нет.
#if defined(_MSC_VER) || defined(__clang__)
#define BENCH_NOINLINE __declspec(noinline)
#else
#define BENCH_NOINLINE __attribute__((noinline))
#endif

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
    WriteText("content_catalog benchmark failed: ");
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

// Дописывает сегмент пути через '/', не допуская переполнения.
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

static BENCH_NOINLINE bool ReadBenchRoot(wchar_t *output, uint32_t capacity)
{
    char utf8[LAIUE_PLATFORM_PATH_CAPACITY];
    uint32_t length = PlatformGetEnvironmentUtf8(BENCH_ROOT_ENV, utf8, sizeof(utf8));
    if (length > 0u)
    {
        return PlatformUtf8ToWide(utf8, length, output, capacity, NULL);
    }
    if (!PlatformExecutableDirectory(output, capacity))
    {
        return false;
    }
    return AppendPath(output, capacity, BENCH_ROOT_FALLBACK);
}

// Создаёт <root>/<scenario>/shaders с count файлами `.ls`; повторный вызов при
// готовом маркере ничего не делает.
static BENCH_NOINLINE bool EnsureDataset(const wchar_t *root, const BenchScenario *scenario,
                                        wchar_t *outScenarioDir, wchar_t *outShadersDir)
{
    if (!PlatformCreateDirectory(root))
    {
        return false;
    }

    CopyText(outScenarioDir, BENCH_PATH_CAPACITY, root);
    if (!AppendPath(outScenarioDir, BENCH_PATH_CAPACITY, scenario->name) ||
        !PlatformCreateDirectory(outScenarioDir))
    {
        return false;
    }

    CopyText(outShadersDir, BENCH_PATH_CAPACITY, outScenarioDir);
    if (!AppendPath(outShadersDir, BENCH_PATH_CAPACITY, L"shaders") ||
        !PlatformCreateDirectory(outShadersDir))
    {
        return false;
    }

    wchar_t marker[BENCH_PATH_CAPACITY];
    CopyText(marker, BENCH_PATH_CAPACITY, outScenarioDir);
    if (!AppendPath(marker, BENCH_PATH_CAPACITY, L"prepared.txt"))
    {
        return false;
    }
    if (PlatformPathExists(marker))
    {
        return true;
    }

    static const uint8_t oneByte[1] = {0u};
    for (uint32_t index = 0u; index < scenario->count; ++index)
    {
        wchar_t name[16];
        name[0] = L's';
        name[1] = L'h';
        name[2] = L'a';
        name[3] = L'd';
        name[4] = L'e';
        name[5] = L'r';
        name[6] = L'_';
        FormatIndex(&name[7], index);
        name[11] = L'.';
        name[12] = L'l';
        name[13] = L's';
        name[14] = L'\0';

        wchar_t path[BENCH_PATH_CAPACITY];
        CopyText(path, BENCH_PATH_CAPACITY, outShadersDir);
        if (!AppendPath(path, BENCH_PATH_CAPACITY, name) ||
            !PlatformWriteEntireFile(path, oneByte, sizeof(oneByte)))
        {
            return false;
        }
    }

    return PlatformWriteEntireFile(marker, oneByte, sizeof(oneByte));
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

static bool ListIsSorted(const LaiueContentList *list)
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
        hash ^= 0x1fU;
        hash *= 1099511628211ULL;
    }
    return hash;
}

// Возвращает лучший (минимальный) замер и общий checksum результатов, чтобы
// baseline и candidate можно было сверить, а вычисление не выбросил компилятор.
static BENCH_NOINLINE double MeasureScenario(const wchar_t *scenarioDir, uint32_t expectedCount,
                                             uint32_t warmup, uint32_t iterations,
                                             uint64_t *outHash)
{
    LaiueContentCatalog *catalog = LaiueContentCatalogCreate(scenarioDir);
    if (catalog == NULL)
    {
        Fail("catalog creation");
    }

    for (uint32_t index = 0u; index < warmup; ++index)
    {
        LaiueContentList list;
        if (!LaiueContentCatalogEnumerate(catalog, LAIUE_CONTENT_SHADER, &list) ||
            list.count != expectedCount)
        {
            Fail("warmup enumeration");
        }
        LaiueContentListRelease(&list);
    }

    double best = 0.0;
    uint64_t hash = 0u;
    for (uint32_t index = 0u; index < iterations; ++index)
    {
        double begin = PlatformMonotonicSeconds();
        LaiueContentList list;
        if (!LaiueContentCatalogEnumerate(catalog, LAIUE_CONTENT_SHADER, &list))
        {
            Fail("enumeration");
        }
        double elapsed = PlatformMonotonicSeconds() - begin;
        if (list.count != expectedCount || !ListIsSorted(&list))
        {
            Fail("enumeration result");
        }
        hash = HashList(&list);
        LaiueContentListRelease(&list);
        if (index == 0u || elapsed < best)
        {
            best = elapsed;
        }
    }
    LaiueContentCatalogDestroy(catalog);
    g_benchSink += hash;
    *outHash = hash;
    return best;
}

LAIUE_TEST_ENTRY(ContentCatalogBenchmarkEntryPoint)
{
    wchar_t root[BENCH_PATH_CAPACITY];
    if (!ReadBenchRoot(root, BENCH_PATH_CAPACITY))
    {
        Fail("benchmark root");
    }

    for (uint32_t index = 0u; index < BENCH_SCENARIO_COUNT; ++index)
    {
        const BenchScenario *scenario = &g_scenarios[index];
        wchar_t scenarioDir[BENCH_PATH_CAPACITY];
        wchar_t shadersDir[BENCH_PATH_CAPACITY];
        if (!EnsureDataset(root, scenario, scenarioDir, shadersDir))
        {
            Fail("dataset creation");
        }

        // Каталог-сценарий содержит только `shaders`, поэтому корнем каталога
        // содержимого служит он же; сам `shadersDir` нужен только для создания.
        (void)shadersDir;
        uint64_t hash = 0u;
        double best =
            MeasureScenario(scenarioDir, scenario->count, scenario->warmup, scenario->iterations,
                            &hash);

        WriteText("content_catalog scenario=");
        for (const wchar_t *character = scenario->name; *character != L'\0'; ++character)
        {
            char ascii[2];
            ascii[0] = (char)(*character & 0x7f);
            ascii[1] = '\0';
            WriteText(ascii);
        }
        WriteText(" entries=");
        WriteUnsigned(scenario->count);
        WriteText(" best_us=");
        WriteFixed(best * 1000000.0);
        WriteText(" hash=");
        WriteUnsigned(hash);
        WriteText("\n");
    }

    WriteText("content_catalog benchmark: done sink=");
    WriteUnsigned(g_benchSink != 0u ? 1u : 0u);
    WriteText("\n");
    LAIUE_TEST_SUCCESS();
}
