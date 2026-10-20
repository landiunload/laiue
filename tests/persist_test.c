#include "persist/persist.h"
#include "platform/system.h"
#include "task/task_pool.h"
#include "test_runtime.h"

#include <stddef.h>
#include <string.h>

#define TEST_APPLICATION UINT64_C(0x00F0C17900000001)
#define BIG_SIZE (3u * 1024u * 1024u + 12345u)

static char rootPath[960];
static char savePath[1000];
static uint8_t *bigData;

static void Expect(bool condition, const char *message)
{
    if (!condition)
    {
        LaiueTestRuntimeWrite("Persist failure: ");
        LaiueTestRuntimeWrite(message);
        LaiueTestRuntimeWrite("\n");
        LaiueTestRuntimeExit(1);
    }
}

static void ExpectStatus(LaiuePersistStatus status, LaiuePersistStatus expected,
                         const char *message)
{
    if (status != expected)
    {
        LaiueTestRuntimeWrite("Persist failure: ");
        LaiueTestRuntimeWrite(message);
        LaiueTestRuntimeWrite(" (got ");
        LaiueTestRuntimeWrite(LaiuePersistStatusName(status));
        LaiueTestRuntimeWrite(", expected ");
        LaiueTestRuntimeWrite(LaiuePersistStatusName(expected));
        LaiueTestRuntimeWrite(")\n");
        LaiueTestRuntimeExit(1);
    }
}

static bool SameText(const char *first, const char *second)
{
    while (*first != '\0' && *first == *second)
    {
        ++first;
        ++second;
    }
    return *first == *second;
}

static void JoinPath(char *output, const char *directory, const char *name)
{
    uint32_t length = 0u;
    while (directory[length] != '\0')
    {
        output[length] = directory[length];
        ++length;
    }
    if (length != 0u && output[length - 1u] != '/' && output[length - 1u] != '\\')
        output[length++] = '/';
    for (uint32_t index = 0u; name[index] != '\0'; ++index)
        output[length++] = name[index];
    output[length] = '\0';
}

static void Suffixed(char *output, const char *path, const char *suffix)
{
    uint32_t length = 0u;
    while (path[length] != '\0')
    {
        output[length] = path[length];
        ++length;
    }
    for (uint32_t index = 0u; suffix[index] != '\0'; ++index)
        output[length++] = suffix[index];
    output[length] = '\0';
}

/* Эталон CRC-32C по определению: побитно, без таблиц. */
static uint32_t ReferenceCrc32c(const uint8_t *bytes, uint64_t size)
{
    uint32_t crc = UINT32_MAX;
    for (uint64_t index = 0u; index < size; ++index)
    {
        crc ^= bytes[index];
        for (uint32_t bit = 0u; bit < 8u; ++bit)
            crc = (crc >> 1u) ^ (UINT32_C(0x82F63B78) & (0u - (crc & 1u)));
    }
    return ~crc;
}

static bool ReadWholeFile(const char *path, uint8_t **outBytes, uint64_t *outSize)
{
    PlatformReadFile file;
    memset(&file, 0, sizeof(file));
    uint64_t size = 0u;
    if (!PlatformFileOpenRead(path, &file, &size))
        return false;
    uint8_t *bytes = PlatformAllocate(size == 0u ? 1u : (size_t)size, false);
    uint32_t read = 0u;
    const bool ok = bytes != NULL && PlatformFileReadAt(&file, 0u, bytes, (uint32_t)size, &read) &&
                    read == size;
    PlatformFileClose(&file);
    if (!ok)
    {
        PlatformFree(bytes);
        return false;
    }
    *outBytes = bytes;
    *outSize = size;
    return true;
}

static bool WriteWholeFile(const char *path, const uint8_t *bytes, uint64_t size)
{
    PlatformWriteFile file;
    memset(&file, 0, sizeof(file));
    if (!PlatformFileCreateWrite(path, &file))
        return false;
    const bool ok = PlatformFileWrite(&file, bytes, (uint32_t)size);
    PlatformFileCloseWrite(&file);
    return ok;
}

static LaiuePersistSectionDesc Section(const char *name, uint32_t version)
{
    LaiuePersistSectionDesc desc;
    memset(&desc, 0, sizeof(desc));
    desc.structSize = sizeof(desc);
    desc.name = name;
    desc.schemaVersion = version;
    return desc;
}

static LaiuePersistDocument *NewDocument(uint32_t applicationVersion)
{
    LaiuePersistDocumentDesc desc = {sizeof(desc), TEST_APPLICATION, applicationVersion};
    LaiuePersistDocument *document = NULL;
    ExpectStatus(LaiuePersistDocumentCreate(&desc, &document), LAIUE_PERSIST_OK, "document create");
    return document;
}

static LaiuePersistSaveDesc SaveDesc(const char *path, uint32_t backups, LaiuePersistCodec codec)
{
    LaiuePersistSaveDesc desc;
    memset(&desc, 0, sizeof(desc));
    desc.structSize = sizeof(desc);
    desc.path = path;
    desc.backupCount = backups;
    desc.codec = codec;
    return desc;
}

static LaiuePersistReader *OpenSave(const char *path, uint32_t backups, LaiuePersistVerify verify,
                                    LaiuePersistOpenReport *report, LaiuePersistStatus expected)
{
    LaiuePersistOpenDesc desc;
    memset(&desc, 0, sizeof(desc));
    desc.structSize = sizeof(desc);
    desc.path = path;
    desc.backupCount = backups;
    desc.expectedApplicationId = TEST_APPLICATION;
    desc.verify = verify;
    LaiuePersistReader *reader = NULL;
    ExpectStatus(LaiuePersistOpen(&desc, &reader, report), expected, "open");
    Expect((reader != NULL) == (expected == LAIUE_PERSIST_OK), "reader presence follows status");
    return reader;
}

static void WritePrimitives(LaiuePersistSectionWriter *writer, uint32_t generation)
{
    Expect(LaiuePersistWriteU8(writer, 0xabu), "u8");
    Expect(LaiuePersistWriteU16(writer, 0xbeefu), "u16");
    Expect(LaiuePersistWriteU32(writer, generation), "u32");
    Expect(LaiuePersistWriteU64(writer, UINT64_C(0x0123456789abcdef)), "u64");
    Expect(LaiuePersistWriteI32(writer, -7), "i32");
    Expect(LaiuePersistWriteI64(writer, INT64_MIN), "i64");
    Expect(LaiuePersistWriteF32(writer, 1.5f), "f32");
    Expect(LaiuePersistWriteF64(writer, -0.25), "f64");
    Expect(LaiuePersistWriteVarU64(writer, 0u), "var 0");
    Expect(LaiuePersistWriteVarU64(writer, 300u), "var 300");
    Expect(LaiuePersistWriteVarU64(writer, UINT64_MAX), "var max");
    Expect(LaiuePersistWriteVarI64(writer, -1), "var -1");
    Expect(LaiuePersistWriteVarI64(writer, INT64_MIN), "var min");
    Expect(LaiuePersistWriteBytes(writer, "city", 4u), "bytes");
}

static void CheckPrimitives(const uint8_t *bytes, uint64_t size, uint32_t generation)
{
    LaiuePersistCursor cursor = LaiuePersistCursorMake(bytes, size);
    Expect(LaiuePersistReadU8(&cursor) == 0xabu, "read u8");
    Expect(LaiuePersistReadU16(&cursor) == 0xbeefu, "read u16");
    Expect(LaiuePersistReadU32(&cursor) == generation, "read u32");
    Expect(LaiuePersistReadU64(&cursor) == UINT64_C(0x0123456789abcdef), "read u64");
    Expect(LaiuePersistReadI32(&cursor) == -7, "read i32");
    Expect(LaiuePersistReadI64(&cursor) == INT64_MIN, "read i64");
    Expect(LaiuePersistReadF32(&cursor) == 1.5f, "read f32");
    Expect(LaiuePersistReadF64(&cursor) == -0.25, "read f64");
    Expect(LaiuePersistReadVarU64(&cursor) == 0u, "read var 0");
    Expect(LaiuePersistReadVarU64(&cursor) == 300u, "read var 300");
    Expect(LaiuePersistReadVarU64(&cursor) == UINT64_MAX, "read var max");
    Expect(LaiuePersistReadVarI64(&cursor) == -1, "read var -1");
    Expect(LaiuePersistReadVarI64(&cursor) == INT64_MIN, "read var min");
    uint64_t length = 0u;
    const uint8_t *text = LaiuePersistReadBytes(&cursor, &length);
    Expect(text != NULL && length == 4u && memcmp(text, "city", 4u) == 0, "read bytes");
    Expect(LaiuePersistCursorFinished(&cursor), "cursor finished exactly");
    Expect(LaiuePersistReadU8(&cursor) == 0u && cursor.failed, "read past end fails sticky");
}

static bool ProduceRegion(void *context, LaiuePersistSectionWriter *writer)
{
    const uint32_t seed = *(const uint32_t *)context;
    for (uint32_t index = 0u; index < 1000u; ++index)
        if (!LaiuePersistWriteVarU64(writer, (uint64_t)index * seed))
            return false;
    return true;
}

static void FillBig(void)
{
    bigData = PlatformAllocate(BIG_SIZE, false);
    Expect(bigData != NULL, "big allocation");
    uint32_t state = 12345u;
    for (uint32_t index = 0u; index < BIG_SIZE; ++index)
    {
        /* Первая половина повторяется (сжимается), вторая — шум. */
        if (index < BIG_SIZE / 2u)
            bigData[index] = (uint8_t)((index / 7u) % 23u);
        else
        {
            state = state * 1664525u + 1013904223u;
            bigData[index] = (uint8_t)(state >> 24u);
        }
    }
}

static uint32_t regionSeed = 7u;

/* Документ поколения generation: основные секции плюс по региону. */
static LaiuePersistDocument *BuildGeneration(uint32_t generation)
{
    LaiuePersistDocument *document = NewDocument(generation);
    LaiuePersistSectionDesc desc = Section("city/meta", 2u);
    desc.flags = LAIUE_PERSIST_SECTION_REQUIRED;
    desc.tag = generation;
    LaiuePersistSectionWriter *writer = NULL;
    ExpectStatus(LaiuePersistDocumentBeginSection(document, &desc, &writer), LAIUE_PERSIST_OK,
                 "begin meta");
    WritePrimitives(writer, generation);
    ExpectStatus(LaiuePersistSectionEnd(writer), LAIUE_PERSIST_OK, "end meta");
    desc = Section("city/terrain", 1u);
    ExpectStatus(LaiuePersistDocumentAddBorrowed(document, &desc, bigData, BIG_SIZE),
                 LAIUE_PERSIST_OK, "borrowed terrain");
    desc = Section("city/region/0/0", 1u);
    desc.tag = UINT64_C(0xffffffff00000000);
    ExpectStatus(LaiuePersistDocumentAddProduced(document, &desc, ProduceRegion, &regionSeed),
                 LAIUE_PERSIST_OK, "produced region");
    desc = Section("empty", 1u);
    ExpectStatus(LaiuePersistDocumentAddBorrowed(document, &desc, NULL, 0u), LAIUE_PERSIST_OK,
                 "empty section");
    return document;
}

static void CheckGeneration(LaiuePersistReader *reader, uint32_t generation)
{
    LaiuePersistFileInfo info;
    memset(&info, 0, sizeof(info));
    info.structSize = sizeof(info);
    Expect(LaiuePersistReaderGetInfo(reader, &info), "file info");
    Expect(info.applicationId == TEST_APPLICATION && info.applicationVersion == generation &&
               info.sectionCount == 4u && info.formatVersion == 1u,
           "file info values");
    static const char *const order[] = {"city/meta", "city/region/0/0", "city/terrain", "empty"};
    for (uint32_t index = 0u; index < 4u; ++index)
    {
        LaiuePersistSectionInfo section;
        memset(&section, 0, sizeof(section));
        section.structSize = sizeof(section);
        Expect(LaiuePersistReaderGetSection(reader, index, &section), "section info");
        Expect(SameText(section.name, order[index]), "directory is sorted by name");
        uint32_t found = UINT32_MAX;
        ExpectStatus(LaiuePersistReaderFind(reader, order[index], &found), LAIUE_PERSIST_OK,
                     "find");
        Expect(found == index, "find returns directory index");
    }
    uint32_t index = 0u;
    ExpectStatus(LaiuePersistReaderFind(reader, "city/missing", &index), LAIUE_PERSIST_NOT_FOUND,
                 "missing section");

    LaiuePersistSectionInfo meta;
    memset(&meta, 0, sizeof(meta));
    meta.structSize = sizeof(meta);
    Expect(LaiuePersistReaderGetSection(reader, 0u, &meta), "meta info");
    Expect(meta.schemaVersion == 2u && meta.flags == LAIUE_PERSIST_SECTION_REQUIRED &&
               meta.tag == generation,
           "meta descriptor");
    uint8_t metaBytes[256];
    ExpectStatus(LaiuePersistReaderRead(reader, 0u, metaBytes, 4u), LAIUE_PERSIST_BUFFER_TOO_SMALL,
                 "short buffer");
    ExpectStatus(LaiuePersistReaderRead(reader, 0u, metaBytes, sizeof(metaBytes)), LAIUE_PERSIST_OK,
                 "read meta");
    CheckPrimitives(metaBytes, meta.rawSize, generation);

    uint8_t *region = PlatformAllocate(8192u, false);
    LaiuePersistSectionInfo regionInfo;
    memset(&regionInfo, 0, sizeof(regionInfo));
    regionInfo.structSize = sizeof(regionInfo);
    Expect(region != NULL && LaiuePersistReaderGetSection(reader, 1u, &regionInfo), "region info");
    Expect(regionInfo.tag == UINT64_C(0xffffffff00000000), "64-bit tag survives");
    ExpectStatus(LaiuePersistReaderRead(reader, 1u, region, 8192u), LAIUE_PERSIST_OK,
                 "read region");
    LaiuePersistCursor cursor = LaiuePersistCursorMake(region, regionInfo.rawSize);
    for (uint32_t value = 0u; value < 1000u; ++value)
        Expect(LaiuePersistReadVarU64(&cursor) == (uint64_t)value * regionSeed, "region value");
    Expect(LaiuePersistCursorFinished(&cursor), "region finished");
    PlatformFree(region);

    uint8_t *terrain = PlatformAllocate(BIG_SIZE, false);
    Expect(terrain != NULL, "terrain buffer");
    ExpectStatus(LaiuePersistReaderRead(reader, 2u, terrain, BIG_SIZE), LAIUE_PERSIST_OK,
                 "read terrain");
    Expect(memcmp(terrain, bigData, BIG_SIZE) == 0, "terrain round trip");
    PlatformFree(terrain);
    ExpectStatus(LaiuePersistReaderRead(reader, 3u, NULL, 0u), LAIUE_PERSIST_OK, "read empty");
}

static void TestNames(void)
{
    LaiuePersistDocument *document = NewDocument(1u);
    static const char *const invalid[] = {
        "", "/a", "a/", "a//b", ".", "..", "a/./b", "a/../b", "a b", "a\\b", "\xd0\xb3",
    };
    for (uint32_t index = 0u; index < sizeof(invalid) / sizeof(invalid[0]); ++index)
    {
        LaiuePersistSectionDesc desc = Section(invalid[index], 1u);
        ExpectStatus(LaiuePersistDocumentAddBorrowed(document, &desc, NULL, 0u),
                     LAIUE_PERSIST_INVALID_ARGUMENT, "invalid name rejected");
    }
    LaiuePersistSectionDesc desc = Section("a.b_c-d/E9/..x", 1u);
    ExpectStatus(LaiuePersistDocumentAddBorrowed(document, &desc, NULL, 0u), LAIUE_PERSIST_OK,
                 "valid name accepted");
    char longest[LAIUE_PERSIST_SECTION_NAME_CAPACITY + 1u];
    memset(longest, 'n', sizeof(longest));
    longest[LAIUE_PERSIST_SECTION_NAME_CAPACITY] = '\0';
    desc = Section(longest, 1u);
    ExpectStatus(LaiuePersistDocumentAddBorrowed(document, &desc, NULL, 0u),
                 LAIUE_PERSIST_INVALID_ARGUMENT, "80-byte name rejected");
    longest[LAIUE_PERSIST_SECTION_NAME_CAPACITY - 1u] = '\0';
    desc = Section(longest, 1u);
    ExpectStatus(LaiuePersistDocumentAddBorrowed(document, &desc, NULL, 0u), LAIUE_PERSIST_OK,
                 "79-byte name accepted");
    desc = Section("flags", 1u);
    desc.flags = 2u;
    ExpectStatus(LaiuePersistDocumentAddBorrowed(document, &desc, NULL, 0u),
                 LAIUE_PERSIST_INVALID_ARGUMENT, "unknown flag rejected");
    /* Пока секция открыта, другую начать нельзя. */
    desc = Section("open", 1u);
    LaiuePersistSectionWriter *writer = NULL;
    ExpectStatus(LaiuePersistDocumentBeginSection(document, &desc, &writer), LAIUE_PERSIST_OK,
                 "begin");
    LaiuePersistSectionWriter *second = NULL;
    ExpectStatus(LaiuePersistDocumentBeginSection(document, &desc, &second),
                 LAIUE_PERSIST_INVALID_ARGUMENT, "nested begin rejected");
    Expect(!LaiuePersistSectionWrite(writer, NULL, 1u), "NULL bytes fail");
    Expect(!LaiuePersistWriteU8(writer, 1u), "failure is sticky");
    ExpectStatus(LaiuePersistSectionEnd(writer), LAIUE_PERSIST_INVALID_ARGUMENT,
                 "end reports sticky failure");
    LaiuePersistSaveDesc save = SaveDesc(savePath, 0u, LAIUE_PERSIST_CODEC_NONE);
    char other[sizeof(savePath)];
    Suffixed(other, savePath, "-names");
    save.path = other;
    ExpectStatus(LaiuePersistSave(document, &save), LAIUE_PERSIST_OK, "save after failed section");
    LaiuePersistReader *reader =
        OpenSave(other, 0u, LAIUE_PERSIST_VERIFY_FULL, NULL, LAIUE_PERSIST_OK);
    LaiuePersistFileInfo info;
    memset(&info, 0, sizeof(info));
    info.structSize = sizeof(info);
    Expect(LaiuePersistReaderGetInfo(reader, &info) && info.sectionCount == 2u,
           "failed section was removed");
    LaiuePersistClose(reader);
    Expect(PlatformFileDeleteUtf8(other), "cleanup names");
    LaiuePersistDocumentDestroy(document);
}

static void TestDuplicate(void)
{
    LaiuePersistDocument *document = NewDocument(1u);
    LaiuePersistSectionDesc desc = Section("twice", 1u);
    ExpectStatus(LaiuePersistDocumentAddBorrowed(document, &desc, "a", 1u), LAIUE_PERSIST_OK,
                 "first");
    ExpectStatus(LaiuePersistDocumentAddBorrowed(document, &desc, "b", 1u), LAIUE_PERSIST_OK,
                 "second");
    char path[sizeof(savePath)];
    Suffixed(path, savePath, "-duplicate");
    LaiuePersistSaveDesc save = SaveDesc(path, 0u, LAIUE_PERSIST_CODEC_LZ);
    ExpectStatus(LaiuePersistSave(document, &save), LAIUE_PERSIST_DUPLICATE_SECTION, "duplicate");
    PlatformPathInformation information;
    Expect(PlatformGetPathInformationUtf8(path, &information) && !information.exists,
           "failed save leaves no file");
    LaiuePersistDocumentDestroy(document);
}

static void SaveGeneration(uint32_t generation, LaiuePersistCodec codec,
                           const LaiueTaskExecutor *executor)
{
    LaiuePersistDocument *document = BuildGeneration(generation);
    LaiuePersistSaveDesc save = SaveDesc(savePath, 2u, codec);
    save.executor = executor;
    ExpectStatus(LaiuePersistSave(document, &save), LAIUE_PERSIST_OK, "save generation");
    LaiuePersistDocumentDestroy(document);
}

static void CheckFileCrcs(const char *path)
{
    uint8_t *bytes = NULL;
    uint64_t size = 0u;
    Expect(ReadWholeFile(path, &bytes, &size), "read file for CRC check");
    Expect(size > 72u, "file has prologue and epilogue");
    uint32_t stored = 0u;
    memcpy(&stored, bytes + 28u, 4u);
    Expect(stored == ReferenceCrc32c(bytes, 28u), "prologue CRC matches reference");
    memcpy(&stored, bytes + size - 4u, 4u);
    Expect(stored == ReferenceCrc32c(bytes + size - 40u, 36u), "epilogue CRC matches reference");
    static const uint8_t check[] = "123456789";
    Expect(ReferenceCrc32c(check, 9u) == UINT32_C(0xE3069283), "reference CRC check value");
    PlatformFree(bytes);
}

static void TestRoundTripAndBackups(const LaiueTaskExecutor *executor)
{
    SaveGeneration(1u, LAIUE_PERSIST_CODEC_NONE, NULL);
    SaveGeneration(2u, LAIUE_PERSIST_CODEC_LZ, NULL);
    SaveGeneration(3u, LAIUE_PERSIST_CODEC_LZ, executor);
    CheckFileCrcs(savePath);

    LaiuePersistReader *reader =
        OpenSave(savePath, 0u, LAIUE_PERSIST_VERIFY_FULL, NULL, LAIUE_PERSIST_OK);
    CheckGeneration(reader, 3u);
    LaiuePersistSectionInfo terrain;
    memset(&terrain, 0, sizeof(terrain));
    terrain.structSize = sizeof(terrain);
    Expect(LaiuePersistReaderGetSection(reader, 2u, &terrain), "terrain info");
    Expect(terrain.codec == LAIUE_PERSIST_CODEC_LZ && terrain.storedSize < terrain.rawSize,
           "compressible terrain shrinks");
    LaiuePersistClose(reader);

    char backup[sizeof(savePath)];
    Suffixed(backup, savePath, ".1");
    reader = OpenSave(backup, 0u, LAIUE_PERSIST_VERIFY_FULL, NULL, LAIUE_PERSIST_OK);
    CheckGeneration(reader, 2u);
    LaiuePersistClose(reader);
    Suffixed(backup, savePath, ".2");
    reader = OpenSave(backup, 0u, LAIUE_PERSIST_VERIFY_FULL, NULL, LAIUE_PERSIST_OK);
    CheckGeneration(reader, 1u);
    LaiuePersistClose(reader);

    /* Четвёртое сохранение вытесняет первое: копий остаётся две. */
    SaveGeneration(4u, LAIUE_PERSIST_CODEC_LZ, executor);
    Suffixed(backup, savePath, ".2");
    reader = OpenSave(backup, 0u, LAIUE_PERSIST_VERIFY_FULL, NULL, LAIUE_PERSIST_OK);
    CheckGeneration(reader, 2u);
    LaiuePersistClose(reader);
    Suffixed(backup, savePath, ".3");
    PlatformPathInformation information;
    Expect(PlatformGetPathInformationUtf8(backup, &information) && !information.exists,
           "no third backup");
    PlatformPathInformation temporary;
    Suffixed(backup, savePath, ".tmp");
    Expect(PlatformGetPathInformationUtf8(backup, &temporary) && !temporary.exists,
           "temporary file is gone");
}

static void TestCorruptionFallback(void)
{
    uint8_t *original = NULL;
    uint64_t size = 0u;
    Expect(ReadWholeFile(savePath, &original, &size), "read main file");

    /* Порча данных секции: структура цела, а полная проверка — нет. */
    uint8_t *damaged = PlatformAllocate((size_t)size, false);
    Expect(damaged != NULL, "damaged copy");
    memcpy(damaged, original, (size_t)size);
    damaged[200] ^= 0x10u;
    Expect(WriteWholeFile(savePath, damaged, size), "write damaged main");
    LaiuePersistReader *reader =
        OpenSave(savePath, 0u, LAIUE_PERSIST_VERIFY_STRUCTURE, NULL, LAIUE_PERSIST_OK);
    LaiuePersistStatus worst = LAIUE_PERSIST_OK;
    for (uint32_t index = 0u; index < 4u; ++index)
    {
        uint8_t *buffer = PlatformAllocate(BIG_SIZE, false);
        const LaiuePersistStatus status = LaiuePersistReaderRead(reader, index, buffer, BIG_SIZE);
        if (status != LAIUE_PERSIST_OK)
            worst = status;
        PlatformFree(buffer);
    }
    ExpectStatus(worst, LAIUE_PERSIST_CORRUPT, "damaged section is detected on read");
    LaiuePersistClose(reader);

    LaiuePersistOpenReport report;
    memset(&report, 0, sizeof(report));
    report.structSize = sizeof(report);
    reader = OpenSave(savePath, 2u, LAIUE_PERSIST_VERIFY_FULL, &report, LAIUE_PERSIST_OK);
    Expect(report.chosenCandidate == 1u && report.candidateStatus[0] == LAIUE_PERSIST_CORRUPT,
           "full verification falls back to the first backup");
    CheckGeneration(reader, 3u);
    LaiuePersistClose(reader);

    /* Обрезанный файл отвергается уже структурной проверкой. */
    Expect(WriteWholeFile(savePath, original, size - 1u), "write truncated main");
    memset(&report, 0, sizeof(report));
    report.structSize = sizeof(report);
    reader = OpenSave(savePath, 2u, LAIUE_PERSIST_VERIFY_STRUCTURE, &report, LAIUE_PERSIST_OK);
    Expect(report.chosenCandidate == 1u, "truncated file falls back");
    LaiuePersistClose(reader);
    OpenSave(savePath, 0u, LAIUE_PERSIST_VERIFY_STRUCTURE, NULL, LAIUE_PERSIST_CORRUPT);

    Expect(WriteWholeFile(savePath, original, size), "restore main");
    PlatformFree(damaged);
    PlatformFree(original);

    LaiuePersistOpenDesc wrong;
    memset(&wrong, 0, sizeof(wrong));
    wrong.structSize = sizeof(wrong);
    wrong.path = savePath;
    wrong.expectedApplicationId = TEST_APPLICATION + 1u;
    LaiuePersistReader *none = NULL;
    ExpectStatus(LaiuePersistOpen(&wrong, &none, NULL), LAIUE_PERSIST_WRONG_APPLICATION,
                 "foreign application rejected");
    char missing[sizeof(savePath)];
    Suffixed(missing, savePath, "-missing");
    wrong.path = missing;
    wrong.expectedApplicationId = 0u;
    ExpectStatus(LaiuePersistOpen(&wrong, &none, NULL), LAIUE_PERSIST_NOT_FOUND, "missing file");
}

/* Каждый байт файла покрыт контрольной суммой: любая порча обнаруживается
 * либо при открытии, либо при чтении, и ни разу не роняет разбор. */
static void TestEveryByteDetected(void)
{
    char path[sizeof(savePath)];
    Suffixed(path, savePath, "-small");
    LaiuePersistDocument *document = NewDocument(9u);
    uint8_t pattern[600];
    for (uint32_t index = 0u; index < sizeof(pattern); ++index)
        pattern[index] = (uint8_t)(index % 13u);
    LaiuePersistSectionDesc desc = Section("lz", 3u);
    ExpectStatus(LaiuePersistDocumentAddBorrowed(document, &desc, pattern, sizeof(pattern)),
                 LAIUE_PERSIST_OK, "lz section");
    desc = Section("raw", 1u);
    ExpectStatus(LaiuePersistDocumentAddBorrowed(document, &desc, "plain", 5u), LAIUE_PERSIST_OK,
                 "raw section");
    LaiuePersistSaveDesc save = SaveDesc(path, 0u, LAIUE_PERSIST_CODEC_LZ);
    ExpectStatus(LaiuePersistSave(document, &save), LAIUE_PERSIST_OK, "save small");
    LaiuePersistDocumentDestroy(document);
    uint8_t *original = NULL;
    uint64_t size = 0u;
    Expect(ReadWholeFile(path, &original, &size), "read small");
    uint8_t *mutated = PlatformAllocate((size_t)size, false);
    uint8_t buffer[sizeof(pattern)];
    for (uint64_t position = 0u; position < size; ++position)
    {
        for (uint32_t bit = 0u; bit < 8u; bit += 3u)
        {
            memcpy(mutated, original, (size_t)size);
            mutated[position] ^= (uint8_t)(1u << bit);
            Expect(WriteWholeFile(path, mutated, size), "write mutation");
            LaiuePersistOpenDesc open;
            memset(&open, 0, sizeof(open));
            open.structSize = sizeof(open);
            open.path = path;
            LaiuePersistReader *reader = NULL;
            bool detected = LaiuePersistOpen(&open, &reader, NULL) != LAIUE_PERSIST_OK;
            if (!detected)
            {
                LaiuePersistFileInfo info;
                memset(&info, 0, sizeof(info));
                info.structSize = sizeof(info);
                Expect(LaiuePersistReaderGetInfo(reader, &info), "mutated info");
                for (uint32_t index = 0u; index < info.sectionCount; ++index)
                    if (LaiuePersistReaderRead(reader, index, buffer, sizeof(buffer)) !=
                        LAIUE_PERSIST_OK)
                        detected = true;
                LaiuePersistClose(reader);
            }
            Expect(detected, "every flipped bit is detected");
        }
    }
    PlatformFree(mutated);
    PlatformFree(original);
    Expect(PlatformFileDeleteUtf8(path), "cleanup small");
}

static void TestCopyRemaining(void)
{
    /* Читатель открыт на тот же файл, который перезаписывается. */
    LaiuePersistReader *reader =
        OpenSave(savePath, 0u, LAIUE_PERSIST_VERIFY_STRUCTURE, NULL, LAIUE_PERSIST_OK);
    LaiuePersistDocument *document = NewDocument(5u);
    LaiuePersistSectionDesc desc = Section("city/meta", 2u);
    desc.tag = 5u;
    desc.flags = LAIUE_PERSIST_SECTION_REQUIRED;
    LaiuePersistSectionWriter *writer = NULL;
    ExpectStatus(LaiuePersistDocumentBeginSection(document, &desc, &writer), LAIUE_PERSIST_OK,
                 "begin");
    WritePrimitives(writer, 5u);
    ExpectStatus(LaiuePersistSectionEnd(writer), LAIUE_PERSIST_OK, "end");
    ExpectStatus(LaiuePersistDocumentCopyRemaining(document, reader), LAIUE_PERSIST_OK,
                 "copy remaining");
    LaiuePersistSaveDesc save = SaveDesc(savePath, 2u, LAIUE_PERSIST_CODEC_LZ);
    ExpectStatus(LaiuePersistSave(document, &save), LAIUE_PERSIST_OK, "incremental save");
    LaiuePersistDocumentDestroy(document);
    LaiuePersistClose(reader);
    reader = OpenSave(savePath, 0u, LAIUE_PERSIST_VERIFY_FULL, NULL, LAIUE_PERSIST_OK);
    CheckGeneration(reader, 5u);
    LaiuePersistClose(reader);
}

static bool MigrateAddCounter(void *context, uint32_t fromVersion, const uint8_t *input,
                              uint64_t inputSize, LaiuePersistSectionWriter *output)
{
    (void)context;
    return LaiuePersistSectionWrite(output, input, inputSize) &&
           LaiuePersistWriteU32(output, fromVersion + 100u);
}

static void TestMigrations(void)
{
    char path[sizeof(savePath)];
    Suffixed(path, savePath, "-migrate");
    LaiuePersistDocument *document = NewDocument(1u);
    LaiuePersistSectionDesc desc = Section("old", 1u);
    ExpectStatus(LaiuePersistDocumentAddBorrowed(document, &desc, "v1", 2u), LAIUE_PERSIST_OK,
                 "old");
    LaiuePersistSaveDesc save = SaveDesc(path, 0u, LAIUE_PERSIST_CODEC_LZ);
    ExpectStatus(LaiuePersistSave(document, &save), LAIUE_PERSIST_OK, "save old");
    LaiuePersistDocumentDestroy(document);
    LaiuePersistReader *reader =
        OpenSave(path, 0u, LAIUE_PERSIST_VERIFY_FULL, NULL, LAIUE_PERSIST_OK);
    const LaiuePersistMigration steps[] = {
        {2u, MigrateAddCounter, NULL},
        {1u, MigrateAddCounter, NULL},
    };
    LaiuePersistBlob blob;
    ExpectStatus(LaiuePersistReaderReadUpgraded(reader, 0u, 3u, steps, 2u, &blob), LAIUE_PERSIST_OK,
                 "upgrade 1 to 3");
    LaiuePersistCursor cursor = LaiuePersistCursorMake(blob.data, blob.size);
    uint8_t text[2];
    Expect(LaiuePersistCursorRead(&cursor, text, 2u) && memcmp(text, "v1", 2u) == 0,
           "payload kept");
    Expect(LaiuePersistReadU32(&cursor) == 101u && LaiuePersistReadU32(&cursor) == 102u &&
               LaiuePersistCursorFinished(&cursor),
           "steps applied in version order");
    LaiuePersistBlobRelease(&blob);
    ExpectStatus(LaiuePersistReaderReadUpgraded(reader, 0u, 1u, NULL, 0u, &blob), LAIUE_PERSIST_OK,
                 "same version needs no steps");
    Expect(blob.size == 2u, "same version payload");
    LaiuePersistBlobRelease(&blob);
    ExpectStatus(LaiuePersistReaderReadUpgraded(reader, 0u, 4u, steps, 2u, &blob),
                 LAIUE_PERSIST_MIGRATION_FAILED, "missing step");
    Expect(blob.data == NULL, "failed upgrade releases the blob");
    ExpectStatus(LaiuePersistReaderReadUpgraded(reader, 0u, 0u, steps, 2u, &blob),
                 LAIUE_PERSIST_UNSUPPORTED_VERSION, "newer schema than supported");
    LaiuePersistClose(reader);
    Expect(PlatformFileDeleteUtf8(path), "cleanup migrate");
}

typedef struct GateState
{
    volatile uint32_t entered;
    volatile uint32_t release;
} GateState;

static bool BlockingProducer(void *context, LaiuePersistSectionWriter *writer)
{
    GateState *gate = context;
    PlatformAtomicStoreU32Release(&gate->entered, 1u);
    while (PlatformAtomicLoadU32Acquire(&gate->release) == 0u)
        PlatformSleepMilliseconds(1u);
    return LaiuePersistWriteU32(writer, 42u);
}

static void TestJobs(void)
{
    /* Отмена: прежний файл остаётся нетронутым. */
    static GateState gate;
    LaiuePersistDocument *document = BuildGeneration(6u);
    LaiuePersistSectionDesc desc = Section("slow", 1u);
    ExpectStatus(LaiuePersistDocumentAddProduced(document, &desc, BlockingProducer, &gate),
                 LAIUE_PERSIST_OK, "slow producer");
    LaiuePersistSaveDesc save = SaveDesc(savePath, 2u, LAIUE_PERSIST_CODEC_LZ);
    LaiuePersistJob *job = NULL;
    ExpectStatus(LaiuePersistSaveStart(document, &save, &job), LAIUE_PERSIST_OK, "save start");
    while (PlatformAtomicLoadU32Acquire(&gate.entered) == 0u)
        PlatformSleepMilliseconds(1u);
    LaiuePersistProgress progress;
    memset(&progress, 0, sizeof(progress));
    progress.structSize = sizeof(progress);
    Expect(LaiuePersistJobPoll(job, &progress) && progress.state == LAIUE_PERSIST_JOB_RUNNING,
           "job is running while producer waits");
    LaiuePersistJobCancel(job);
    PlatformAtomicStoreU32Release(&gate.release, 1u);
    ExpectStatus(LaiuePersistJobWait(job), LAIUE_PERSIST_CANCELLED, "cancelled save");
    Expect(LaiuePersistJobPoll(job, &progress) && progress.state == LAIUE_PERSIST_JOB_CANCELLED,
           "cancelled state");
    LaiuePersistJobDestroy(job);
    LaiuePersistReader *reader =
        OpenSave(savePath, 0u, LAIUE_PERSIST_VERIFY_FULL, NULL, LAIUE_PERSIST_OK);
    CheckGeneration(reader, 5u);
    LaiuePersistClose(reader);

    /* Обычное фоновое сохранение. */
    document = BuildGeneration(7u);
    ExpectStatus(LaiuePersistSaveStart(document, &save, &job), LAIUE_PERSIST_OK, "save start 2");
    ExpectStatus(LaiuePersistJobWait(job), LAIUE_PERSIST_OK, "background save");
    Expect(LaiuePersistJobPoll(job, &progress) && progress.state == LAIUE_PERSIST_JOB_SUCCEEDED &&
               progress.sectionsDone == 4u && progress.sectionsTotal == 4u &&
               progress.bytesDone == progress.bytesTotal && progress.bytesDone > BIG_SIZE,
           "save progress is complete");
    LaiuePersistJobDestroy(job);

    /* Ошибка аргумента всё равно забирает документ. */
    document = BuildGeneration(8u);
    ExpectStatus(LaiuePersistSaveStart(document, NULL, &job), LAIUE_PERSIST_INVALID_ARGUMENT,
                 "invalid start consumes document");
}

typedef struct LoadState
{
    uint32_t sections;
    uint64_t bytes;
    bool failAt;
} LoadState;

static bool SkipTerrain(void *context, const LaiuePersistSectionInfo *section)
{
    (void)context;
    return !SameText(section->name, "city/terrain");
}

static bool CountSection(void *context, const LaiuePersistSectionInfo *section,
                         const uint8_t *bytes, uint64_t size)
{
    LoadState *state = context;
    Expect(!SameText(section->name, "city/terrain"), "filtered section is not delivered");
    if (SameText(section->name, "city/meta"))
        CheckPrimitives(bytes, size, 7u);
    ++state->sections;
    state->bytes += size;
    return !state->failAt;
}

static void TestLoadJob(void)
{
    LoadState state;
    memset(&state, 0, sizeof(state));
    LaiuePersistLoadDesc desc;
    memset(&desc, 0, sizeof(desc));
    desc.structSize = sizeof(desc);
    desc.open.structSize = sizeof(desc.open);
    desc.open.path = savePath;
    desc.open.backupCount = 2u;
    desc.open.expectedApplicationId = TEST_APPLICATION;
    desc.open.verify = LAIUE_PERSIST_VERIFY_FULL;
    desc.filter = SkipTerrain;
    desc.consumer = CountSection;
    desc.context = &state;
    LaiuePersistJob *job = NULL;
    ExpectStatus(LaiuePersistLoadStart(&desc, &job), LAIUE_PERSIST_OK, "load start");
    ExpectStatus(LaiuePersistJobWait(job), LAIUE_PERSIST_OK, "load finished");
    LaiuePersistProgress progress;
    memset(&progress, 0, sizeof(progress));
    progress.structSize = sizeof(progress);
    Expect(LaiuePersistJobPoll(job, &progress) && progress.state == LAIUE_PERSIST_JOB_SUCCEEDED &&
               progress.sectionsDone == 4u && progress.bytesDone == progress.bytesTotal,
           "load progress is complete");
    Expect(state.sections == 3u, "three sections delivered");
    LaiuePersistJobDestroy(job);

    memset(&state, 0, sizeof(state));
    state.failAt = true;
    ExpectStatus(LaiuePersistLoadStart(&desc, &job), LAIUE_PERSIST_OK, "load start 2");
    ExpectStatus(LaiuePersistJobWait(job), LAIUE_PERSIST_CONSUMER_FAILED, "consumer stops load");
    Expect(state.sections == 1u, "load stops at the first refusal");
    LaiuePersistJobDestroy(job);
}

static void Cleanup(void)
{
    static const char *const suffixes[] = {"", ".1", ".2", ".3", ".tmp"};
    char path[sizeof(savePath)];
    for (uint32_t index = 0u; index < sizeof(suffixes) / sizeof(suffixes[0]); ++index)
    {
        Suffixed(path, savePath, suffixes[index]);
        Expect(PlatformFileDeleteUtf8(path), "cleanup file");
    }
    static wchar_t wide[LAIUE_PLATFORM_PATH_CAPACITY];
    uint32_t length = 0u;
    while (rootPath[length] != '\0')
        ++length;
    Expect(PlatformUtf8ToWide(rootPath, length, wide, LAIUE_PLATFORM_PATH_CAPACITY, NULL) &&
               PlatformRemoveDirectory(wide),
           "cleanup directory");
}

LAIUE_TEST_ENTRY(PersistTestEntryPoint)
{
    static wchar_t executable[LAIUE_PLATFORM_PATH_CAPACITY];
    static char directory[900];
    Expect(PlatformExecutableDirectory(executable, LAIUE_PLATFORM_PATH_CAPACITY) &&
               PlatformWideToUtf8(executable, directory, sizeof(directory), NULL),
           "executable directory");
    uint8_t random[6];
    Expect(PlatformRandomBytes(random, sizeof(random)), "random name");
    char name[] = "persist_test_000000000000";
    static const char digits[] = "0123456789abcdef";
    for (uint32_t index = 0u; index < sizeof(random); ++index)
    {
        name[13u + index * 2u] = digits[random[index] >> 4u];
        name[14u + index * 2u] = digits[random[index] & 15u];
    }
    JoinPath(rootPath, directory, name);
    static wchar_t wideRoot[LAIUE_PLATFORM_PATH_CAPACITY];
    uint32_t length = 0u;
    while (rootPath[length] != '\0')
        ++length;
    Expect(PlatformUtf8ToWide(rootPath, length, wideRoot, LAIUE_PLATFORM_PATH_CAPACITY, NULL) &&
               PlatformCreateDirectory(wideRoot),
           "create root");
    JoinPath(savePath, rootPath, "city.sav");
    FillBig();

    LaiueTaskPool *pool = LaiueTaskPoolCreate(4u);
    Expect(pool != NULL, "task pool");
    LaiueTaskExecutor executor;
    memset(&executor, 0, sizeof(executor));
    executor.structSize = sizeof(executor);
    Expect(LaiueTaskPoolGetExecutor(pool, &executor), "executor");

    TestNames();
    TestDuplicate();
    TestRoundTripAndBackups(&executor);
    TestCorruptionFallback();
    TestEveryByteDetected();
    TestCopyRemaining();
    TestMigrations();
    TestJobs();
    TestLoadJob();

    LaiueTaskPoolDestroy(pool);
    PlatformFree(bigData);
    Cleanup();
    LaiueTestRuntimeWrite("persist: passed\n");
    LAIUE_TEST_SUCCESS();
}
