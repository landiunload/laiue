// Ручной benchmark сохранений: в ALL не входит, в CTest не регистрируется.
//
// «Город» на 256 МиБ: 1024 региона по 256 КиБ, половина данных хорошо
// сжимается (как сетки зданий и дорог), половина — шум. Меряется то, что
// важно для кадра: сколько главный поток тратит на сбор документа (здесь
// borrowed-секции, то есть почти ноль), и отдельно — сколько длится фоновая
// запись и чтение. Запись включает fsync, поэтому зависит от диска.

#include "persist/persist.h"
#include "platform/system.h"
#include "task/task_pool.h"
#include "test_runtime.h"

#include <stddef.h>
#include <string.h>

#define REGIONS 1024u
#define REGION_BYTES (256u * 1024u)

static void WriteUnsigned(uint64_t value)
{
    char digits[21];
    uint32_t length = 0u;
    do
    {
        digits[length++] = (char)('0' + value % 10u);
        value /= 10u;
    } while (value != 0u);
    char text[22];
    for (uint32_t index = 0u; index < length; ++index)
        text[index] = digits[length - 1u - index];
    text[length] = '\0';
    LaiueTestRuntimeWrite(text);
}

static void Report(const char *label, double seconds, uint64_t bytes)
{
    LaiueTestRuntimeWrite(label);
    LaiueTestRuntimeWrite(": ");
    WriteUnsigned((uint64_t)(seconds * 1000.0));
    LaiueTestRuntimeWrite(" ms");
    if (bytes != 0u && seconds > 0.0)
    {
        LaiueTestRuntimeWrite(", ");
        WriteUnsigned((uint64_t)((double)bytes / seconds / (1024.0 * 1024.0)));
        LaiueTestRuntimeWrite(" MiB/s");
    }
    LaiueTestRuntimeWrite("\n");
}

static void Fail(const char *message, LaiuePersistStatus status)
{
    LaiueTestRuntimeWrite(message);
    LaiueTestRuntimeWrite(": ");
    LaiueTestRuntimeWrite(LaiuePersistStatusName(status));
    LaiueTestRuntimeWrite("\n");
    LaiueTestRuntimeExit(1);
}

static void RegionName(uint32_t index, char name[32])
{
    static const char prefix[] = "region/";
    memcpy(name, prefix, sizeof(prefix) - 1u);
    uint32_t length = sizeof(prefix) - 1u;
    char digits[10];
    uint32_t count = 0u;
    do
    {
        digits[count++] = (char)('0' + index % 10u);
        index /= 10u;
    } while (index != 0u);
    while (count != 0u)
        name[length++] = digits[--count];
    name[length] = '\0';
}

LAIUE_TEST_ENTRY(PersistBenchmarkEntryPoint)
{
    static char path[1000];
    wchar_t executable[512];
    if (!PlatformExecutableDirectory(executable, 512u) ||
        !PlatformWideToUtf8(executable, path, 900u, NULL))
        Fail("executable directory", 0u);
    uint32_t length = 0u;
    while (path[length] != '\0')
        ++length;
    memcpy(path + length, "/persist_benchmark.sav", sizeof("/persist_benchmark.sav"));

    uint8_t *data = PlatformAllocate((size_t)REGIONS * REGION_BYTES, false);
    if (data == NULL)
        Fail("allocation", LAIUE_PERSIST_OUT_OF_MEMORY);
    uint32_t state = 1u;
    for (uint64_t index = 0u; index < (uint64_t)REGIONS * REGION_BYTES; ++index)
    {
        if ((index / REGION_BYTES) % 2u == 0u)
            data[index] = (uint8_t)((index / 13u) % 17u);
        else
        {
            state = state * 1664525u + 1013904223u;
            data[index] = (uint8_t)(state >> 24u);
        }
    }
    const uint64_t total = (uint64_t)REGIONS * REGION_BYTES;

    const uint32_t threads =
        LaiueTaskLogicalProcessorCount() > 64u ? 64u : LaiueTaskLogicalProcessorCount();
    LaiueTaskPool *pool = LaiueTaskPoolCreate(threads);
    LaiueTaskExecutor executor;
    memset(&executor, 0, sizeof(executor));
    executor.structSize = sizeof(executor);
    if (pool == NULL || !LaiueTaskPoolGetExecutor(pool, &executor))
        Fail("pool", 0u);

    static const LaiuePersistCodec codecs[] = {LAIUE_PERSIST_CODEC_NONE, LAIUE_PERSIST_CODEC_LZ,
                                               LAIUE_PERSIST_CODEC_LZ};
    static const char *const names[] = {"save raw", "save LZ 1 thread", "save LZ all threads"};
    for (uint32_t variant = 0u; variant < 3u; ++variant)
    {
        double begin = PlatformMonotonicSeconds();
        LaiuePersistDocumentDesc documentDesc = {sizeof(documentDesc), 1u, 1u};
        LaiuePersistDocument *document = NULL;
        LaiuePersistStatus status = LaiuePersistDocumentCreate(&documentDesc, &document);
        for (uint32_t region = 0u; region < REGIONS && status == LAIUE_PERSIST_OK; ++region)
        {
            char name[32];
            RegionName(region, name);
            LaiuePersistSectionDesc section = {sizeof(section), name, 1u, 0u, region};
            status = LaiuePersistDocumentAddBorrowed(
                document, &section, data + (size_t)region * REGION_BYTES, REGION_BYTES);
        }
        if (status != LAIUE_PERSIST_OK)
            Fail("document", status);
        Report("  main thread: build document", PlatformMonotonicSeconds() - begin, 0u);
        LaiuePersistSaveDesc save;
        memset(&save, 0, sizeof(save));
        save.structSize = sizeof(save);
        save.path = path;
        save.codec = codecs[variant];
        save.executor = variant == 2u ? &executor : NULL;
        LaiuePersistJob *job = NULL;
        begin = PlatformMonotonicSeconds();
        status = LaiuePersistSaveStart(document, &save, &job);
        const double startCost = PlatformMonotonicSeconds() - begin;
        if (status != LAIUE_PERSIST_OK)
            Fail("save start", status);
        status = LaiuePersistJobWait(job);
        const double elapsed = PlatformMonotonicSeconds() - begin;
        LaiuePersistJobDestroy(job);
        if (status != LAIUE_PERSIST_OK)
            Fail("save", status);
        Report("  main thread: start job", startCost, 0u);
        Report(names[variant], elapsed, total);

        LaiuePersistOpenDesc open;
        memset(&open, 0, sizeof(open));
        open.structSize = sizeof(open);
        open.path = path;
        LaiuePersistReader *reader = NULL;
        begin = PlatformMonotonicSeconds();
        status = LaiuePersistOpen(&open, &reader, NULL);
        if (status != LAIUE_PERSIST_OK)
            Fail("open", status);
        LaiuePersistFileInfo info;
        memset(&info, 0, sizeof(info));
        info.structSize = sizeof(info);
        (void)LaiuePersistReaderGetInfo(reader, &info);
        uint8_t *buffer = PlatformAllocate(REGION_BYTES, false);
        uint64_t checksum = 0u;
        for (uint32_t region = 0u; region < info.sectionCount; ++region)
        {
            status = LaiuePersistReaderRead(reader, region, buffer, REGION_BYTES);
            if (status != LAIUE_PERSIST_OK)
                Fail("read", status);
            checksum += buffer[region % REGION_BYTES];
        }
        const double readTime = PlatformMonotonicSeconds() - begin;
        LaiuePersistClose(reader);
        PlatformFree(buffer);
        Report("  load", readTime, total);
        LaiueTestRuntimeWrite("  file size ");
        WriteUnsigned(info.fileSize / (1024u * 1024u));
        LaiueTestRuntimeWrite(" MiB, checksum ");
        WriteUnsigned(checksum);
        LaiueTestRuntimeWrite("\n");
    }
    (void)PlatformFileDeleteUtf8(path);
    LaiueTaskPoolDestroy(pool);
    PlatformFree(data);
    LAIUE_TEST_SUCCESS();
}
