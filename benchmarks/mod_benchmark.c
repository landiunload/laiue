// Ручной benchmark discovery и манифеста нативных модов. В ALL не входит и в
// CTest не регистрируется: его запускают осознанно и читают глазами.
//
// Модуль `mod` не работает в покадровом пути, поэтому меряется сама стоимость
// операций, а не FPS: повторный разбор манифеста, обход каталога паков и
// прямой Inspect одного пака (он тоже пересканирует root ради проверки
// ASCII case-collision). Нагрузка одинакова для baseline и candidate: имена
// паков и содержимое манифестов детерминированы и задаются только индексом.
//
// Строка вывода: `modbench,<workload>,<iterations>,<elapsed_ns>,<checksum>`.
// Целое число наносекунд выбрано намеренно: печать double без CRT была бы
// ещё одним источником расхождения, а сравнивать нужно только результат.
//
// Root для паков берётся из переменной окружения LAIUE_MOD_BENCH_ROOT, чтобы
// baseline и candidate в A/B-прогоне работали на одном наборе файлов; без неё
// используется каталог исполняемого файла.

#include "mod/mod_host.h"
#include "mod/mod_manifest.h"
#include "platform/system.h"
#include "test_runtime.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define MODBENCH_TYPICAL_PACKS 32u
#define MODBENCH_MANY_PACKS 1024u
#define MODBENCH_PARSE_ITERATIONS 100000u
#define MODBENCH_TYPICAL_ITERATIONS 1000u
#define MODBENCH_MANY_ITERATIONS 10u
#define MODBENCH_INSPECT_TYPICAL_ITERATIONS 2000u
#define MODBENCH_INSPECT_MANY_ITERATIONS 1000u
#define MODBENCH_WARMUP_DIVISOR 20u

#if defined(_WIN32) && (defined(_M_ARM64) || defined(__aarch64__))
#define MODBENCH_ENTRY_KEY "entry_windows_arm64"
#define MODBENCH_ARTIFACT_SUFFIX L".dll"
#define MODBENCH_ARTIFACT_SUFFIX_UTF8 ".dll"
#elif defined(_WIN32)
#define MODBENCH_ENTRY_KEY "entry_windows_x86_64"
#define MODBENCH_ARTIFACT_SUFFIX L".dll"
#define MODBENCH_ARTIFACT_SUFFIX_UTF8 ".dll"
#elif defined(__linux__) && defined(__aarch64__) && defined(LAIUE_LINUX_LIBC_MUSL)
#define MODBENCH_ENTRY_KEY "entry_linux_arm64_musl"
#define MODBENCH_ARTIFACT_SUFFIX L".so"
#define MODBENCH_ARTIFACT_SUFFIX_UTF8 ".so"
#elif defined(__linux__) && defined(__aarch64__)
#define MODBENCH_ENTRY_KEY "entry_linux_arm64_gnu"
#define MODBENCH_ARTIFACT_SUFFIX L".so"
#define MODBENCH_ARTIFACT_SUFFIX_UTF8 ".so"
#elif defined(LAIUE_LINUX_LIBC_MUSL)
#define MODBENCH_ENTRY_KEY "entry_linux_x86_64_musl"
#define MODBENCH_ARTIFACT_SUFFIX L".so"
#define MODBENCH_ARTIFACT_SUFFIX_UTF8 ".so"
#elif defined(__linux__)
#define MODBENCH_ENTRY_KEY "entry_linux_x86_64_gnu"
#define MODBENCH_ARTIFACT_SUFFIX L".so"
#define MODBENCH_ARTIFACT_SUFFIX_UTF8 ".so"
#elif defined(__APPLE__) && defined(__x86_64__)
#define MODBENCH_ENTRY_KEY "entry_macos_x86_64"
#define MODBENCH_ARTIFACT_SUFFIX L".dylib"
#define MODBENCH_ARTIFACT_SUFFIX_UTF8 ".dylib"
#elif defined(__APPLE__) && (defined(__aarch64__) || defined(__arm64__))
#define MODBENCH_ENTRY_KEY "entry_macos_arm64"
#define MODBENCH_ARTIFACT_SUFFIX L".dylib"
#define MODBENCH_ARTIFACT_SUFFIX_UTF8 ".dylib"
#else
#error Unsupported mod benchmark platform
#endif

typedef struct ModBenchScratch
{
    wchar_t packPath[LAIUE_PLATFORM_PATH_CAPACITY];
    wchar_t manifestPath[LAIUE_PLATFORM_PATH_CAPACITY];
    wchar_t artifactPath[LAIUE_PLATFORM_PATH_CAPACITY];
    wchar_t packName[LAIUE_MOD_NATIVE_NAME_CAPACITY];
    wchar_t artifactLeaf[LAIUE_MOD_NATIVE_NAME_CAPACITY];
    char manifest[512];
} ModBenchScratch;

static volatile uint64_t g_sink;

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

static void Report(const char *name, uint32_t iterations, double elapsed, uint64_t checksum)
{
    uint64_t nanoseconds = elapsed > 0.0 ? (uint64_t)(elapsed * 1000000000.0 + 0.5) : 0u;
    WriteText("modbench,");
    WriteText(name);
    WriteText(",");
    WriteUnsigned(iterations);
    WriteText(",");
    WriteUnsigned(nanoseconds);
    WriteText(",");
    WriteUnsigned(checksum);
    WriteText("\n");
}

static bool Join(wchar_t *output, uint32_t capacity, const wchar_t *first, const wchar_t *second,
                 const wchar_t *third)
{
    uint32_t length = 0;
    const wchar_t *parts[] = {first, second, third};
    for (uint32_t partIndex = 0; partIndex < 3u; ++partIndex)
    {
        const wchar_t *part = parts[partIndex];
        if (part == NULL || part[0] == L'\0')
        {
            continue;
        }
        if (length > 0u && output[length - 1u] != L'/' && output[length - 1u] != L'\\')
        {
            if (length + 1u >= capacity)
            {
                return false;
            }
            output[length++] = L'/';
        }
        for (uint32_t index = 0; part[index] != L'\0'; ++index)
        {
            if (length + 1u >= capacity)
            {
                return false;
            }
            output[length++] = part[index];
        }
    }
    output[length] = L'\0';
    return true;
}

static bool AppendText(char *buffer, uint32_t *length, uint32_t capacity, const char *text)
{
    for (uint32_t index = 0; text[index] != '\0'; ++index)
    {
        if (*length + 1u >= capacity)
        {
            return false;
        }
        buffer[(*length)++] = text[index];
    }
    return true;
}

static bool AppendUnsigned(char *buffer, uint32_t *length, uint32_t capacity, uint32_t value,
                           uint32_t minimumDigits)
{
    char digits[10];
    uint32_t digitCount = 0u;
    if (value == 0u)
    {
        digits[digitCount++] = '0';
    }
    while (value != 0u)
    {
        digits[digitCount++] = (char)('0' + (value % 10u));
        value /= 10u;
    }
    while (digitCount < minimumDigits)
    {
        digits[digitCount++] = '0';
    }
    for (uint32_t index = 0u; index < digitCount; ++index)
    {
        if (*length + 1u >= capacity)
        {
            return false;
        }
        buffer[(*length)++] = digits[digitCount - index - 1u];
    }
    return true;
}

static bool BuildPackName(wchar_t *output, uint32_t capacity, uint32_t index, const wchar_t *suffix)
{
    uint32_t length = 0u;
    const wchar_t *prefix = L"bench_";
    for (uint32_t index2 = 0u; prefix[index2] != L'\0'; ++index2)
    {
        if (length + 1u >= capacity)
        {
            return false;
        }
        output[length++] = prefix[index2];
    }
    wchar_t digits[8];
    uint32_t digitCount = 0u;
    uint32_t value = index;
    if (value == 0u)
    {
        digits[digitCount++] = L'0';
    }
    while (value != 0u)
    {
        digits[digitCount++] = (wchar_t)(L'0' + (value % 10u));
        value /= 10u;
    }
    while (digitCount < 4u)
    {
        digits[digitCount++] = L'0';
    }
    for (uint32_t digit = 0u; digit < digitCount; ++digit)
    {
        if (length + 1u >= capacity)
        {
            return false;
        }
        output[length++] = digits[digitCount - digit - 1u];
    }
    for (uint32_t index2 = 0u; suffix[index2] != L'\0'; ++index2)
    {
        if (length + 1u >= capacity)
        {
            return false;
        }
        output[length++] = suffix[index2];
    }
    output[length] = L'\0';
    return true;
}

// Создаёт один пак: каталог, mod.lm (все платформенные ключи) и
// минимальный regular-file native artifact. Артефакт не загружается,
// поэтому его содержимое не важно — важен только тип и размер > 0.
static bool CreatePack(const wchar_t *root, ModBenchScratch *scratch, uint32_t index,
                       wchar_t *outPackName)
{
    if (!BuildPackName(scratch->packName, LAIUE_MOD_NATIVE_NAME_CAPACITY, index, L".lmp") ||
        !BuildPackName(scratch->artifactLeaf, LAIUE_MOD_NATIVE_NAME_CAPACITY, index,
                       MODBENCH_ARTIFACT_SUFFIX) ||
        !Join(scratch->packPath, LAIUE_PLATFORM_PATH_CAPACITY, root, scratch->packName, NULL) ||
        !Join(scratch->manifestPath, LAIUE_PLATFORM_PATH_CAPACITY, root, scratch->packName,
              LAIUE_MOD_MANIFEST_FILE_NAME) ||
        !Join(scratch->artifactPath, LAIUE_PLATFORM_PATH_CAPACITY, root, scratch->packName,
              scratch->artifactLeaf))
    {
        return false;
    }

    // Пак создаётся один раз. PlatformWriteEntireFile делает fsync, и полный
    // пересозданный набор из тысячи паков стоил бы десятки секунд на каждый
    // A/B-прогон. Пропуск уже готового пака сохраняет одинаковую нагрузку
    // для baseline и candidate: они делят один root.
    if (PlatformPathExists(scratch->manifestPath) && PlatformPathExists(scratch->artifactPath))
    {
        if (outPackName != NULL)
        {
            for (uint32_t index2 = 0u; index2 < LAIUE_MOD_NATIVE_NAME_CAPACITY; ++index2)
            {
                outPackName[index2] = scratch->packName[index2];
                if (scratch->packName[index2] == L'\0')
                {
                    break;
                }
            }
        }
        return true;
    }

    uint32_t length = 0u;
    if (!AppendText(scratch->manifest, &length, (uint32_t)sizeof(scratch->manifest),
                    "LAIUE MOD 3\nid = bench.mod") ||
        !AppendUnsigned(scratch->manifest, &length, (uint32_t)sizeof(scratch->manifest), index,
                        4u) ||
        !AppendText(scratch->manifest, &length, (uint32_t)sizeof(scratch->manifest),
                    "\nname = Bench Pack\nversion = 1.0.0\nengine = 0.7\n\n[native]\nabi = "
                    "1\n" MODBENCH_ENTRY_KEY " = bench_") ||
        !AppendUnsigned(scratch->manifest, &length, (uint32_t)sizeof(scratch->manifest), index,
                        4u) ||
        !AppendText(scratch->manifest, &length, (uint32_t)sizeof(scratch->manifest),
                    MODBENCH_ARTIFACT_SUFFIX_UTF8 "\n"))
    {
        return false;
    }

    if (!PlatformCreateDirectory(scratch->packPath) ||
        !PlatformWriteEntireFile(scratch->manifestPath, scratch->manifest, length))
    {
        return false;
    }

    uint8_t byte = (uint8_t)('M' + (index & 0x0fu));
    if (!PlatformWriteEntireFile(scratch->artifactPath, &byte, 1u))
    {
        return false;
    }

    if (outPackName != NULL)
    {
        for (uint32_t index2 = 0u; index2 < LAIUE_MOD_NATIVE_NAME_CAPACITY; ++index2)
        {
            outPackName[index2] = scratch->packName[index2];
            if (scratch->packName[index2] == L'\0')
            {
                break;
            }
        }
    }
    return true;
}

static void Fail(const char *message)
{
    WriteText("mod benchmark failure: ");
    WriteText(message);
    WriteText("\n");
    LaiueTestRuntimeExit(1);
}

// Представительный манифест со всеми платформенными ключами: разбор
// проверяет и devirtualization ключей, и копирование UTF-8/identifier.
static const char g_manifestText[] = "LAIUE MOD 3\n"
                                     "id = bench.sample\n"
                                     "name = Bench Sample\n"
                                     "version = 1.2.0-beta\n"
                                     "engine = 0.7\n"
                                     "\n"
                                     "[native]\n"
                                     "abi = 1\n"
                                     "entry_windows_x86_64 = bench.windows-x86_64.dll\n"
                                     "entry_windows_arm64 = bench.windows-arm64.dll\n"
                                     "entry_linux_x86_64_gnu = bench.linux-x86_64-gnu.so\n"
                                     "entry_linux_x86_64_musl = bench.linux-x86_64-musl.so\n"
                                     "entry_linux_arm64_gnu = bench.linux-arm64-gnu.so\n"
                                     "entry_linux_arm64_musl = bench.linux-arm64-musl.so\n"
                                     "entry_macos_x86_64 = bench.macos-x86_64.dylib\n"
                                     "entry_macos_arm64 = bench.macos-arm64.dylib\n";

static double MeasureManifestParse(uint32_t iterations)
{
    uint64_t checksum = 0u;
    double begin = PlatformMonotonicSeconds();
    for (uint32_t sample = 0u; sample < iterations; ++sample)
    {
        LaiueModManifest manifest;
        LaiueModDiagnostic diagnostic;
        if (LaiueModManifestParse(g_manifestText, sizeof(g_manifestText) - 1u, &manifest,
                                  &diagnostic) != LAIUE_MOD_STATUS_OK)
        {
            Fail("representative manifest was rejected");
        }
        checksum += (uint64_t)(uint8_t)manifest.id[0] + manifest.requiredEngineMinor;
    }
    g_sink += checksum;
    return PlatformMonotonicSeconds() - begin;
}

static double MeasureEnumerate(const wchar_t *root, uint32_t iterations)
{
    uint64_t checksum = 0u;
    double begin = PlatformMonotonicSeconds();
    for (uint32_t sample = 0u; sample < iterations; ++sample)
    {
        LaiueModPackList list;
        LaiueModDiagnostic diagnostic;
        if (LaiueModPackEnumerate(root, &list, &diagnostic) != LAIUE_MOD_STATUS_OK)
        {
            Fail("pack enumeration failed");
        }
        checksum += list.count;
        if (list.count > 0u)
        {
            checksum += (uint64_t)(uint8_t)list.entries[list.count - 1u].packName[0];
        }
        LaiueModPackListRelease(&list);
    }
    g_sink += checksum;
    return PlatformMonotonicSeconds() - begin;
}

static double MeasureInspect(const wchar_t *root, const wchar_t *packName, uint32_t iterations)
{
    uint64_t checksum = 0u;
    double begin = PlatformMonotonicSeconds();
    for (uint32_t sample = 0u; sample < iterations; ++sample)
    {
        LaiueModPackInfo info;
        LaiueModDiagnostic diagnostic;
        if (LaiueModPackInspect(root, packName, &info, &diagnostic) != LAIUE_MOD_STATUS_OK)
        {
            Fail("direct pack inspection failed");
        }
        checksum += (uint64_t)(uint8_t)info.packName[0];
    }
    g_sink += checksum;
    return PlatformMonotonicSeconds() - begin;
}

LAIUE_TEST_ENTRY(ModBenchmarkEntryPoint)
{
    ModBenchScratch *scratch = PlatformAllocate(sizeof(*scratch), false);
    if (scratch == NULL)
    {
        Fail("could not allocate benchmark scratch");
    }
    wchar_t *executableDirectory =
        PlatformAllocate((size_t)LAIUE_PLATFORM_PATH_CAPACITY * sizeof(wchar_t), false);
    wchar_t *typicalRoot =
        PlatformAllocate((size_t)LAIUE_PLATFORM_PATH_CAPACITY * sizeof(wchar_t), false);
    wchar_t *manyRoot =
        PlatformAllocate((size_t)LAIUE_PLATFORM_PATH_CAPACITY * sizeof(wchar_t), false);
    wchar_t *baseRoot =
        PlatformAllocate((size_t)LAIUE_PLATFORM_PATH_CAPACITY * sizeof(wchar_t), false);
    if (executableDirectory == NULL || typicalRoot == NULL || manyRoot == NULL || baseRoot == NULL)
    {
        Fail("could not allocate benchmark paths");
    }

    char *environmentPath = PlatformAllocate((size_t)LAIUE_PLATFORM_PATH_CAPACITY * 4u, false);
    if (executableDirectory == NULL || typicalRoot == NULL || manyRoot == NULL ||
        baseRoot == NULL || environmentPath == NULL)
    {
        Fail("could not allocate benchmark paths");
    }

    uint32_t environmentLength = PlatformGetEnvironmentUtf8(
        "LAIUE_MOD_BENCH_ROOT", environmentPath, (uint32_t)(LAIUE_PLATFORM_PATH_CAPACITY * 4u));
    uint32_t wideLength = 0u;
    if (environmentLength > 0u &&
        PlatformUtf8ToWide(environmentPath, environmentLength, baseRoot,
                           LAIUE_PLATFORM_PATH_CAPACITY, &wideLength) &&
        wideLength > 0u)
    {
        // baseRoot уже заполнен явным корнем из окружения.
    }
    else if (!PlatformExecutableDirectory(baseRoot, LAIUE_PLATFORM_PATH_CAPACITY))
    {
        Fail("could not obtain executable directory");
    }
    if (!Join(executableDirectory, LAIUE_PLATFORM_PATH_CAPACITY, baseRoot, L"mod_bench_packs",
              NULL) ||
        !Join(typicalRoot, LAIUE_PLATFORM_PATH_CAPACITY, executableDirectory, L"typical", NULL) ||
        !Join(manyRoot, LAIUE_PLATFORM_PATH_CAPACITY, executableDirectory, L"many", NULL))
    {
        Fail("benchmark root path overflowed");
    }
    if (!PlatformCreateDirectory(baseRoot) || !PlatformCreateDirectory(executableDirectory) ||
        !PlatformCreateDirectory(typicalRoot) || !PlatformCreateDirectory(manyRoot))
    {
        Fail("could not create benchmark roots");
    }

    for (uint32_t index = 0u; index < MODBENCH_TYPICAL_PACKS; ++index)
    {
        if (!CreatePack(typicalRoot, scratch, index, NULL))
        {
            Fail("could not create typical pack");
        }
    }
    wchar_t inspectedPack[LAIUE_MOD_NATIVE_NAME_CAPACITY];
    for (uint32_t index = 0u; index < MODBENCH_MANY_PACKS; ++index)
    {
        if (!CreatePack(manyRoot, scratch, index, index == 0u ? inspectedPack : NULL))
        {
            Fail("could not create many pack");
        }
    }

    // Прогрев: файловый кэш и внутренние ветви прогреваются один раз, чтобы
    // первый измеренный прогон не мерил разогрев.
    (void)MeasureManifestParse(MODBENCH_PARSE_ITERATIONS / MODBENCH_WARMUP_DIVISOR);
    (void)MeasureEnumerate(typicalRoot, MODBENCH_TYPICAL_ITERATIONS / MODBENCH_WARMUP_DIVISOR);
    (void)MeasureEnumerate(manyRoot, MODBENCH_MANY_ITERATIONS / MODBENCH_WARMUP_DIVISOR + 1u);
    (void)MeasureInspect(typicalRoot, L"bench_0000.lmp",
                         MODBENCH_INSPECT_TYPICAL_ITERATIONS / MODBENCH_WARMUP_DIVISOR);
    (void)MeasureInspect(manyRoot, inspectedPack,
                         MODBENCH_INSPECT_MANY_ITERATIONS / MODBENCH_WARMUP_DIVISOR);

    Report("manifest_parse", MODBENCH_PARSE_ITERATIONS,
           MeasureManifestParse(MODBENCH_PARSE_ITERATIONS), g_sink);
    Report("enumerate_typical", MODBENCH_TYPICAL_ITERATIONS,
           MeasureEnumerate(typicalRoot, MODBENCH_TYPICAL_ITERATIONS), g_sink);
    Report("enumerate_many", MODBENCH_MANY_ITERATIONS,
           MeasureEnumerate(manyRoot, MODBENCH_MANY_ITERATIONS), g_sink);
    Report("inspect_typical", MODBENCH_INSPECT_TYPICAL_ITERATIONS,
           MeasureInspect(typicalRoot, L"bench_0000.lmp", MODBENCH_INSPECT_TYPICAL_ITERATIONS),
           g_sink);
    Report("inspect_many", MODBENCH_INSPECT_MANY_ITERATIONS,
           MeasureInspect(manyRoot, inspectedPack, MODBENCH_INSPECT_MANY_ITERATIONS), g_sink);

    WriteText("mod benchmark done sink=");
    WriteUnsigned(g_sink != 0u ? 1u : 0u);
    WriteText("\n");

    PlatformFree(baseRoot);
    PlatformFree(manyRoot);
    PlatformFree(typicalRoot);
    PlatformFree(executableDirectory);
    PlatformFree(environmentPath);
    PlatformFree(scratch);
    LAIUE_TEST_SUCCESS();
}
