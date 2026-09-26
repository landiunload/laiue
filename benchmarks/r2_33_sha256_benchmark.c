// Ручной A/B-стенд SHA-256 (src/platform/sha256.c). В ALL и CTest не входит и
// запускается осознанно. Скрипт A/B гоняет один и тот же исполняемый файл
// (baseline и candidate собраны из одного и того же исходника стенда) и
// сравнивает строки CASE по total_ns.
//
// Предмет: LaiueSha256Compute. Производственный файл компилируется прямо в
// исполняемый файл, как это делают другие стенды закрытых/внутренних TU:
// отдельного публичного символа для шедулера нет, а сам API — one-shot.
//
// На каждый размер печатается:
//
//   CASE <name> size <n> reps <r> ns <total> digest <64 hex>
//
// digest — полный 32-байтный результат, посчитанный вне замера: он обязан
// совпасть у baseline и candidate (криптографический результат byte-exact).
// total_ns — сумма reps замеренных вызовов. Финальные строки:
//
//   TOTAL_NS <sum>  CHECKSUM <hex>  MEM commit=... peak_commit=... peak_working=...
//
// reps фиксированы на размер (не зависят от машины), иначе один и тот же
// размер нельзя было бы сравнивать между запусками.

#include "platform/sha256.h"
#include "platform/system.h"
#include "test_runtime.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if defined(_WIN32)
// K32GetProcessMemoryInfo уже экспортируется kernel32, который линкует
// обёртка standalone-исполняемого файла; psapi.lib не требуется.
typedef struct R2Sha256MemoryCounters
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
} R2Sha256MemoryCounters;

__declspec(dllimport) int __stdcall K32GetProcessMemoryInfo(
    void *process, R2Sha256MemoryCounters *counters, uint32_t size);
#endif

typedef struct R2Sha256Case
{
    const char *name;
    uint32_t size;
    uint32_t reps;
} R2Sha256Case;

// Размеры из задания: пустое сообщение, однобайтное, границы одного блока
// (55/56/63/64/65), соседние границы 127/128/129, 1 KiB и 1 MiB.
static const R2Sha256Case kCases[] = {
    {"size0", 0u, 400000u},
    {"size1", 1u, 400000u},
    {"size55", 55u, 400000u},
    {"size56", 56u, 300000u},
    {"size63", 63u, 300000u},
    {"size64", 64u, 300000u},
    {"size65", 65u, 300000u},
    {"size127", 127u, 250000u},
    {"size128", 128u, 250000u},
    {"size129", 129u, 250000u},
    {"size1KiB", 1024u, 30000u},
    {"size1MiB", 1048576u, 120u},
};
#define R2_SHA256_CASE_COUNT (sizeof(kCases) / sizeof(kCases[0]))

static const char kHexDigits[] = "0123456789abcdef";

static uint64_t g_combinedChecksum = 0xcbf29ce484222325ull;
static uint64_t g_totalNs;
static volatile uint64_t g_sink;

static void WriteText(const char *text)
{
    LaiueTestRuntimeWrite(text);
}

static void WriteUnsigned(uint64_t value)
{
    char reversed[24];
    uint32_t length = 0u;
    if (value == 0u)
    {
        reversed[length++] = '0';
    }
    while (value != 0u)
    {
        reversed[length++] = (char)('0' + (value % 10u));
        value /= 10u;
    }
    char text[25];
    for (uint32_t index = 0u; index < length; ++index)
    {
        text[index] = reversed[length - index - 1u];
    }
    text[length] = '\0';
    WriteText(text);
}

static void WriteHexBytes(const uint8_t *bytes, uint32_t size)
{
    char text[65];
    for (uint32_t index = 0u; index < size; ++index)
    {
        text[index * 2u] = kHexDigits[bytes[index] >> 4u];
        text[index * 2u + 1u] = kHexDigits[bytes[index] & 0x0fu];
    }
    text[size * 2u] = '\0';
    WriteText(text);
}

static void WriteHex64(uint64_t value)
{
    char text[19];
    text[0] = '0';
    text[1] = 'x';
    for (uint32_t index = 0u; index < 16u; ++index)
    {
        text[2u + index] = kHexDigits[(value >> (60u - index * 4u)) & 0x0fu];
    }
    text[18] = '\0';
    WriteText(text);
}

static void PrintPeakMemory(void)
{
#if defined(_WIN32)
    R2Sha256MemoryCounters counters;
    memset(&counters, 0, sizeof(counters));
    counters.cb = (uint32_t)sizeof(counters);
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), &counters,
                                (uint32_t)sizeof(counters)) != 0)
    {
        WriteText("MEM commit=");
        WriteUnsigned(counters.pagefileUsage);
        WriteText(" peak_commit=");
        WriteUnsigned(counters.peakPagefileUsage);
        WriteText(" peak_working=");
        WriteUnsigned(counters.peakWorkingSetSize);
        WriteText("\n");
        return;
    }
#endif
    WriteText("MEM unavailable\n");
}

// Детерминированное неслучайное заполнение: одинаково у baseline и candidate,
// без зависимости от libc.
static void FillDeterministic(uint8_t *bytes, uint32_t size, uint32_t seed)
{
    uint32_t state = seed * 2654435761u + 1u;
    for (uint32_t index = 0u; index < size; ++index)
    {
        state = state * 1664525u + 1013904223u;
        bytes[index] = (uint8_t)(state >> 24u);
    }
}

static void RunCase(const R2Sha256Case *benchCase)
{
    uint32_t size = benchCase->size;
    uint32_t capacity = size == 0u ? 1u : size;
    uint8_t *input = (uint8_t *)PlatformAllocate((size_t)capacity, false);
    if (input == NULL)
    {
        WriteText("CASE ");
        WriteText(benchCase->name);
        WriteText(" ALLOC_FAIL\n");
        return;
    }
    FillDeterministic(input, size, size);

    uint8_t digest[32];
    if (!LaiueSha256Compute(input, size, digest))
    {
        WriteText("CASE ");
        WriteText(benchCase->name);
        WriteText(" COMPUTE_FAIL\n");
        PlatformFree(input);
        return;
    }

    // Прогрев вне статистики: код и страницы горячие к первому замеру.
    uint8_t warm[32];
    for (uint32_t index = 0u; index < 32u; ++index)
    {
        if (!LaiueSha256Compute(input, size, warm))
        {
            WriteText("CASE ");
            WriteText(benchCase->name);
            WriteText(" COMPUTE_FAIL\n");
            PlatformFree(input);
            return;
        }
    }

    uint64_t sink = 0u;
    uint32_t reps = benchCase->reps;
    double started = PlatformMonotonicSeconds();
    for (uint32_t index = 0u; index < reps; ++index)
    {
        uint8_t out[32];
        if (!LaiueSha256Compute(input, size, out))
        {
            WriteText("CASE ");
            WriteText(benchCase->name);
            WriteText(" COMPUTE_FAIL\n");
            PlatformFree(input);
            return;
        }
        // Дёшево, но не даёт компилятору выбросить вызов.
        sink ^= out[0];
    }
    double finished = PlatformMonotonicSeconds();
    g_sink ^= sink;

    uint64_t totalNs = (uint64_t)((finished - started) * 1000000000.0);
    g_totalNs += totalNs;

    for (uint32_t index = 0u; index < 32u; ++index)
    {
        g_combinedChecksum ^= digest[index];
        g_combinedChecksum *= 0x100000001b3ull;
    }

    WriteText("CASE ");
    WriteText(benchCase->name);
    WriteText(" size ");
    WriteUnsigned(size);
    WriteText(" reps ");
    WriteUnsigned(reps);
    WriteText(" ns ");
    WriteUnsigned(totalNs);
    WriteText(" digest ");
    WriteHexBytes(digest, 32u);
    WriteText("\n");

    PlatformFree(input);
}

LAIUE_TEST_ENTRY(R2Sha256BenchmarkEntryPoint)
{
    for (uint32_t index = 0u; index < R2_SHA256_CASE_COUNT; ++index)
    {
        RunCase(&kCases[index]);
    }
    WriteText("TOTAL_NS ");
    WriteUnsigned(g_totalNs);
    WriteText("\n");
    WriteText("CHECKSUM ");
    WriteHex64(g_combinedChecksum);
    WriteText("\n");
    PrintPeakMemory();
    LaiueTestRuntimeExit(0);
}
