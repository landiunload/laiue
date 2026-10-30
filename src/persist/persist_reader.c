#include "persist/persist_crc.h"
#include "persist/persist_internal.h"

#include <string.h>

const PersistEntry *PersistReaderEntry(const LaiuePersistReader *reader, uint32_t index)
{
    return reader == NULL || index >= reader->count ? NULL : &reader->entries[index];
}

static LaiuePersistStatus ReadExact(LaiuePersistReader *reader, uint64_t offset, void *buffer,
                                    uint32_t count)
{
    uint32_t read = 0u;
    PlatformMutexLock(&reader->mutex);
    const bool ok = PlatformFileReadAt(&reader->file, offset, buffer, count, &read);
    PlatformMutexUnlock(&reader->mutex);
    if (!ok)
        return LAIUE_PERSIST_IO_ERROR;
    /* Файл короче, чем обещает оглавление: его обрезали после открытия. */
    return read == count ? LAIUE_PERSIST_OK : LAIUE_PERSIST_CORRUPT;
}

LaiuePersistStatus PersistReaderReadStored(LaiuePersistReader *reader, const PersistEntry *entry,
                                           uint64_t offset, void *buffer, uint32_t count)
{
    if (offset > entry->storedSize || count > entry->storedSize - offset)
        return LAIUE_PERSIST_CORRUPT;
    return ReadExact(reader, entry->offset + offset, buffer, count);
}

static void CloseReader(LaiuePersistReader *reader)
{
    PlatformFileClose(&reader->file);
    PlatformMutexDestroy(&reader->mutex);
    PlatformFree(reader->entries);
    PlatformFree(reader);
}

static LaiuePersistStatus ParseEntry(const uint8_t *bytes, uint64_t directoryOffset,
                                     PersistEntry *entry)
{
    memset(entry, 0, sizeof(*entry));
    memcpy(entry->name, bytes, LAIUE_PERSIST_SECTION_NAME_CAPACITY);
    uint32_t length = 0u;
    if (entry->name[LAIUE_PERSIST_SECTION_NAME_CAPACITY - 1u] != '\0' ||
        !PersistNameValid(entry->name, &length))
        return LAIUE_PERSIST_CORRUPT;
    for (uint32_t index = length; index < LAIUE_PERSIST_SECTION_NAME_CAPACITY; ++index)
        if (entry->name[index] != '\0')
            return LAIUE_PERSIST_CORRUPT;
    entry->offset = PersistLoadU64(bytes + 80u);
    entry->storedSize = PersistLoadU64(bytes + 88u);
    entry->rawSize = PersistLoadU64(bytes + 96u);
    entry->schemaVersion = PersistLoadU32(bytes + 104u);
    const uint32_t flags = PersistLoadU32(bytes + 108u);
    entry->storedCrc = PersistLoadU32(bytes + 112u);
    entry->rawCrc = PersistLoadU32(bytes + 116u);
    entry->tag = PersistLoadU64(bytes + 120u);
    if ((flags >> 16u) != 0u)
        return LAIUE_PERSIST_UNSUPPORTED_VERSION;
    entry->flags = flags & PERSIST_SECTION_FLAG_MASK;
    entry->codec = (flags >> 8u) & 0xffu;
    if (entry->offset < PERSIST_PROLOGUE_SIZE || entry->offset > directoryOffset ||
        entry->storedSize > directoryOffset - entry->offset)
        return LAIUE_PERSIST_CORRUPT;
    if (entry->codec == LAIUE_PERSIST_CODEC_NONE)
        return entry->storedSize == entry->rawSize ? LAIUE_PERSIST_OK : LAIUE_PERSIST_CORRUPT;
    if (entry->codec != LAIUE_PERSIST_CODEC_LZ)
        return LAIUE_PERSIST_UNSUPPORTED_VERSION;
    /* Блоков столько, сколько нужно для rawSize, и у каждого заголовок. */
    const uint64_t blocks = entry->rawSize / LAIUE_PERSIST_LZ_BLOCK_SIZE +
                            (entry->rawSize % LAIUE_PERSIST_LZ_BLOCK_SIZE != 0u);
    if (entry->storedSize / PERSIST_BLOCK_HEADER_SIZE < blocks)
        return LAIUE_PERSIST_CORRUPT;
    return LAIUE_PERSIST_OK;
}

static LaiuePersistStatus VerifyStoredCrc(LaiuePersistReader *reader, const PersistEntry *entry,
                                          uint8_t *buffer, PersistProgressCounters *progress)
{
    uint32_t crc = 0u;
    for (uint64_t offset = 0u; offset < entry->storedSize;)
    {
        if (progress != NULL && PlatformAtomicLoadU32Acquire(&progress->cancel) != 0u)
            return LAIUE_PERSIST_CANCELLED;
        uint64_t part = entry->storedSize - offset;
        if (part > PERSIST_IO_CHUNK)
            part = PERSIST_IO_CHUNK;
        const LaiuePersistStatus status =
            PersistReaderReadStored(reader, entry, offset, buffer, (uint32_t)part);
        if (status != LAIUE_PERSIST_OK)
            return status;
        crc = LaiuePersistCrc32c(crc, buffer, part);
        offset += part;
    }
    return crc == entry->storedCrc ? LAIUE_PERSIST_OK : LAIUE_PERSIST_CORRUPT;
}

static LaiuePersistStatus OpenSingle(const char *path, const LaiuePersistOpenDesc *desc,
                                     LaiuePersistReader **outReader,
                                     PersistProgressCounters *progress)
{
    LaiuePersistReader *reader = PlatformAllocate(sizeof(*reader), true);
    if (reader == NULL)
        return LAIUE_PERSIST_OUT_OF_MEMORY;
    if (!PlatformMutexInitialize(&reader->mutex))
    {
        PlatformFree(reader);
        return LAIUE_PERSIST_OUT_OF_MEMORY;
    }
    if (!PlatformFileOpenRead(path, &reader->file, &reader->fileSize))
    {
        PlatformPathInformation information;
        const bool missing =
            PlatformGetPathInformationUtf8(path, &information) && !information.exists;
        CloseReader(reader);
        return missing ? LAIUE_PERSIST_NOT_FOUND : LAIUE_PERSIST_IO_ERROR;
    }
    LaiuePersistStatus status = LAIUE_PERSIST_CORRUPT;
    uint8_t prologue[PERSIST_PROLOGUE_SIZE];
    uint8_t epilogue[PERSIST_EPILOGUE_SIZE];
    if (reader->fileSize < PERSIST_PROLOGUE_SIZE + PERSIST_EPILOGUE_SIZE)
        goto fail;
    status = ReadExact(reader, 0u, prologue, sizeof(prologue));
    if (status != LAIUE_PERSIST_OK)
        goto fail;
    status = LAIUE_PERSIST_CORRUPT;
    if (PersistLoadU32(prologue) != PERSIST_PROLOGUE_MAGIC_0 ||
        PersistLoadU32(prologue + 4u) != PERSIST_PROLOGUE_MAGIC_1 ||
        PersistLoadU32(prologue + 28u) != LaiuePersistCrc32c(0u, prologue, 28u))
        goto fail;
    reader->formatVersion = PersistLoadU32(prologue + 8u);
    if (reader->formatVersion != PERSIST_FORMAT_VERSION)
    {
        status = LAIUE_PERSIST_UNSUPPORTED_VERSION;
        goto fail;
    }
    if (PersistLoadU32(prologue + 12u) != PERSIST_PROLOGUE_SIZE)
        goto fail;
    reader->applicationId = PersistLoadU64(prologue + 16u);
    reader->applicationVersion = PersistLoadU32(prologue + 24u);
    if (desc->expectedApplicationId != 0u && desc->expectedApplicationId != reader->applicationId)
    {
        status = LAIUE_PERSIST_WRONG_APPLICATION;
        goto fail;
    }
    status =
        ReadExact(reader, reader->fileSize - PERSIST_EPILOGUE_SIZE, epilogue, sizeof(epilogue));
    if (status != LAIUE_PERSIST_OK)
        goto fail;
    status = LAIUE_PERSIST_CORRUPT;
    const uint64_t directoryOffset = PersistLoadU64(epilogue);
    const uint64_t directorySize = PersistLoadU64(epilogue + 8u);
    const uint64_t recordedSize = PersistLoadU64(epilogue + 16u);
    const uint32_t count = PersistLoadU32(epilogue + 24u);
    const uint32_t directoryCrc = PersistLoadU32(epilogue + 28u);
    if (PersistLoadU32(epilogue + 32u) != PERSIST_EPILOGUE_MAGIC ||
        PersistLoadU32(epilogue + 36u) != LaiuePersistCrc32c(0u, epilogue, 36u) ||
        recordedSize != reader->fileSize || count > LAIUE_PERSIST_MAXIMUM_SECTIONS ||
        directorySize != (uint64_t)count * PERSIST_ENTRY_SIZE ||
        directoryOffset < PERSIST_PROLOGUE_SIZE ||
        directoryOffset > reader->fileSize - PERSIST_EPILOGUE_SIZE ||
        directorySize != reader->fileSize - PERSIST_EPILOGUE_SIZE - directoryOffset)
        goto fail;
    uint8_t *buffer = PlatformAllocate(PERSIST_IO_CHUNK, false);
    if (buffer == NULL)
    {
        status = LAIUE_PERSIST_OUT_OF_MEMORY;
        goto fail;
    }
    if (count != 0u)
    {
        reader->entries = PlatformAllocate((size_t)count * sizeof(*reader->entries), false);
        if (reader->entries == NULL)
        {
            PlatformFree(buffer);
            status = LAIUE_PERSIST_OUT_OF_MEMORY;
            goto fail;
        }
    }
    reader->count = count;
    const uint32_t entriesPerChunk = PERSIST_IO_CHUNK / PERSIST_ENTRY_SIZE;
    uint32_t crc = 0u;
    status = LAIUE_PERSIST_OK;
    for (uint32_t first = 0u; first < count && status == LAIUE_PERSIST_OK; first += entriesPerChunk)
    {
        const uint32_t batch = count - first < entriesPerChunk ? count - first : entriesPerChunk;
        status = ReadExact(reader, directoryOffset + (uint64_t)first * PERSIST_ENTRY_SIZE, buffer,
                           batch * PERSIST_ENTRY_SIZE);
        if (status != LAIUE_PERSIST_OK)
            break;
        crc = LaiuePersistCrc32c(crc, buffer, (uint64_t)batch * PERSIST_ENTRY_SIZE);
        for (uint32_t index = 0u; index < batch && status == LAIUE_PERSIST_OK; ++index)
        {
            PersistEntry *entry = &reader->entries[first + index];
            status =
                ParseEntry(buffer + (size_t)index * PERSIST_ENTRY_SIZE, directoryOffset, entry);
            if (status == LAIUE_PERSIST_OK && first + index != 0u &&
                PersistNameCompare(entry[-1].name, entry->name) >= 0)
                status = LAIUE_PERSIST_CORRUPT;
        }
    }
    if (status == LAIUE_PERSIST_OK && crc != directoryCrc)
        status = LAIUE_PERSIST_CORRUPT;
    if (status == LAIUE_PERSIST_OK && desc->verify == LAIUE_PERSIST_VERIFY_FULL)
    {
        for (uint32_t index = 0u; index < count && status == LAIUE_PERSIST_OK; ++index)
        {
            status = VerifyStoredCrc(reader, &reader->entries[index], buffer, progress);
            if (progress != NULL)
                (void)PlatformAtomicAddI64(&progress->bytesDone,
                                           (int64_t)reader->entries[index].storedSize);
        }
    }
    PlatformFree(buffer);
    if (status != LAIUE_PERSIST_OK)
        goto fail;
    *outReader = reader;
    return LAIUE_PERSIST_OK;
fail:
    CloseReader(reader);
    return status;
}

LaiuePersistStatus PersistOpenRun(const LaiuePersistOpenDesc *desc, LaiuePersistReader **outReader,
                                  LaiuePersistOpenReport *outReport,
                                  PersistProgressCounters *progress)
{
    if (outReader != NULL)
        *outReader = NULL;
    if (outReport != NULL)
    {
        if (outReport->structSize < sizeof(*outReport))
            return LAIUE_PERSIST_INVALID_ARGUMENT;
        const uint32_t structSize = outReport->structSize;
        memset(outReport, 0, sizeof(*outReport));
        outReport->structSize = structSize;
    }
    if (desc == NULL || outReader == NULL || desc->structSize < sizeof(*desc) ||
        desc->path == NULL || desc->backupCount > LAIUE_PERSIST_MAXIMUM_BACKUPS ||
        desc->verify > LAIUE_PERSIST_VERIFY_FULL)
        return LAIUE_PERSIST_INVALID_ARGUMENT;
    char path[PERSIST_PATH_CAPACITY];
    LaiuePersistStatus first = LAIUE_PERSIST_NOT_FOUND;
    for (uint32_t candidate = 0u; candidate <= desc->backupCount; ++candidate)
    {
        char suffix[PERSIST_PATH_SUFFIX_CAPACITY] = {0};
        if (candidate != 0u)
            PersistBackupSuffix(candidate, suffix);
        if (!PersistBuildPath(desc->path, suffix, path))
            return LAIUE_PERSIST_INVALID_ARGUMENT;
        const LaiuePersistStatus status = OpenSingle(path, desc, outReader, progress);
        if (outReport != NULL)
            outReport->candidateStatus[candidate] = status;
        if (status == LAIUE_PERSIST_OK)
        {
            if (outReport != NULL)
                outReport->chosenCandidate = candidate;
            return LAIUE_PERSIST_OK;
        }
        if (status == LAIUE_PERSIST_CANCELLED || status == LAIUE_PERSIST_OUT_OF_MEMORY)
            return status;
        if (first == LAIUE_PERSIST_NOT_FOUND)
            first = status;
    }
    return first;
}

LaiuePersistStatus LaiuePersistOpen(const LaiuePersistOpenDesc *desc,
                                    LaiuePersistReader **outReader,
                                    LaiuePersistOpenReport *outReport)
{
    return PersistOpenRun(desc, outReader, outReport, NULL);
}

void LaiuePersistClose(LaiuePersistReader *reader)
{
    if (reader != NULL)
        CloseReader(reader);
}

bool LaiuePersistReaderGetInfo(const LaiuePersistReader *reader, LaiuePersistFileInfo *outInfo)
{
    if (reader == NULL || outInfo == NULL || outInfo->structSize < sizeof(*outInfo))
        return false;
    outInfo->formatVersion = reader->formatVersion;
    outInfo->applicationId = reader->applicationId;
    outInfo->applicationVersion = reader->applicationVersion;
    outInfo->sectionCount = reader->count;
    outInfo->fileSize = reader->fileSize;
    return true;
}

static void FillSectionInfo(const PersistEntry *entry, LaiuePersistSectionInfo *info)
{
    memcpy(info->name, entry->name, sizeof(info->name));
    info->schemaVersion = entry->schemaVersion;
    info->flags = entry->flags;
    info->codec = entry->codec;
    info->tag = entry->tag;
    info->rawSize = entry->rawSize;
    info->storedSize = entry->storedSize;
}

bool LaiuePersistReaderGetSection(const LaiuePersistReader *reader, uint32_t index,
                                  LaiuePersistSectionInfo *outInfo)
{
    const PersistEntry *entry = PersistReaderEntry(reader, index);
    if (entry == NULL || outInfo == NULL || outInfo->structSize < sizeof(*outInfo))
        return false;
    FillSectionInfo(entry, outInfo);
    return true;
}

LaiuePersistStatus LaiuePersistReaderFind(const LaiuePersistReader *reader, const char *name,
                                          uint32_t *outIndex)
{
    uint32_t length = 0u;
    if (reader == NULL || outIndex == NULL || !PersistNameValid(name, &length))
        return LAIUE_PERSIST_INVALID_ARGUMENT;
    char padded[LAIUE_PERSIST_SECTION_NAME_CAPACITY] = {0};
    memcpy(padded, name, length);
    uint32_t low = 0u;
    uint32_t high = reader->count;
    while (low < high)
    {
        const uint32_t middle = low + (high - low) / 2u;
        const int order = PersistNameCompare(reader->entries[middle].name, padded);
        if (order == 0)
        {
            *outIndex = middle;
            return LAIUE_PERSIST_OK;
        }
        if (order < 0)
            low = middle + 1u;
        else
            high = middle;
    }
    return LAIUE_PERSIST_NOT_FOUND;
}

static LaiuePersistStatus ReadLz(LaiuePersistReader *reader, const PersistEntry *entry,
                                 uint8_t *output, uint32_t *storedCrc)
{
    uint8_t *block = PlatformAllocate(LAIUE_PERSIST_LZ_BOUND(LAIUE_PERSIST_LZ_BLOCK_SIZE), false);
    if (block == NULL)
        return LAIUE_PERSIST_OUT_OF_MEMORY;
    LaiuePersistStatus status = LAIUE_PERSIST_OK;
    uint64_t stored = 0u;
    uint64_t produced = 0u;
    uint32_t crc = 0u;
    while (produced < entry->rawSize)
    {
        uint8_t header[PERSIST_BLOCK_HEADER_SIZE];
        status = PersistReaderReadStored(reader, entry, stored, header, sizeof(header));
        if (status != LAIUE_PERSIST_OK)
            break;
        crc = LaiuePersistCrc32c(crc, header, sizeof(header));
        stored += sizeof(header);
        const uint32_t rawLength = PersistLoadU32(header);
        const uint32_t storedWord = PersistLoadU32(header + 4u);
        const bool uncompressed = (storedWord & PERSIST_BLOCK_UNCOMPRESSED) != 0u;
        const uint32_t storedLength = storedWord & ~PERSIST_BLOCK_UNCOMPRESSED;
        const uint64_t expected = entry->rawSize - produced < LAIUE_PERSIST_LZ_BLOCK_SIZE
                                      ? entry->rawSize - produced
                                      : LAIUE_PERSIST_LZ_BLOCK_SIZE;
        if (rawLength != expected ||
            (uncompressed ? storedLength != rawLength
                          : storedLength == 0u || storedLength >= rawLength))
        {
            status = LAIUE_PERSIST_CORRUPT;
            break;
        }
        uint8_t *target = uncompressed ? output + produced : block;
        status = PersistReaderReadStored(reader, entry, stored, target, storedLength);
        if (status != LAIUE_PERSIST_OK)
            break;
        crc = LaiuePersistCrc32c(crc, target, storedLength);
        stored += storedLength;
        if (!uncompressed &&
            !LaiuePersistLzDecompress(block, storedLength, output + produced, rawLength))
        {
            status = LAIUE_PERSIST_CORRUPT;
            break;
        }
        produced += rawLength;
    }
    PlatformFree(block);
    if (status == LAIUE_PERSIST_OK && stored != entry->storedSize)
        status = LAIUE_PERSIST_CORRUPT;
    *storedCrc = crc;
    return status;
}

LaiuePersistStatus LaiuePersistReaderRead(LaiuePersistReader *reader, uint32_t index, void *buffer,
                                          uint64_t capacity)
{
    const PersistEntry *entry = PersistReaderEntry(reader, index);
    if (entry == NULL || (buffer == NULL && entry->rawSize != 0u))
        return LAIUE_PERSIST_INVALID_ARGUMENT;
    if (capacity < entry->rawSize)
        return LAIUE_PERSIST_BUFFER_TOO_SMALL;
    uint8_t *output = buffer;
    uint32_t storedCrc = 0u;
    LaiuePersistStatus status = LAIUE_PERSIST_OK;
    if (entry->codec == LAIUE_PERSIST_CODEC_NONE)
    {
        for (uint64_t offset = 0u; offset < entry->rawSize && status == LAIUE_PERSIST_OK;)
        {
            uint64_t part = entry->rawSize - offset;
            if (part > PERSIST_IO_CHUNK)
                part = PERSIST_IO_CHUNK;
            status =
                PersistReaderReadStored(reader, entry, offset, output + offset, (uint32_t)part);
            offset += part;
        }
        if (status == LAIUE_PERSIST_OK)
            storedCrc = LaiuePersistCrc32c(0u, output, entry->rawSize);
    }
    else
        status = ReadLz(reader, entry, output, &storedCrc);
    if (status != LAIUE_PERSIST_OK)
        return status;
    if (storedCrc != entry->storedCrc)
        return LAIUE_PERSIST_CORRUPT;
    const uint32_t rawCrc = entry->codec == LAIUE_PERSIST_CODEC_NONE
                                ? storedCrc
                                : LaiuePersistCrc32c(0u, output, entry->rawSize);
    return rawCrc == entry->rawCrc ? LAIUE_PERSIST_OK : LAIUE_PERSIST_CORRUPT;
}

static bool BlobFromChunks(PersistChunks *chunks, LaiuePersistBlob *blob)
{
    if (chunks->size > SIZE_MAX)
        return false;
    uint8_t *data = PlatformAllocate(chunks->size == 0u ? 1u : (size_t)chunks->size, false);
    if (data == NULL)
        return false;
    for (uint64_t offset = 0u; offset < chunks->size; offset += LAIUE_PERSIST_LZ_BLOCK_SIZE)
    {
        const uint64_t part = chunks->size - offset < LAIUE_PERSIST_LZ_BLOCK_SIZE
                                  ? chunks->size - offset
                                  : LAIUE_PERSIST_LZ_BLOCK_SIZE;
        memcpy(data + offset, chunks->chunks[offset / LAIUE_PERSIST_LZ_BLOCK_SIZE], (size_t)part);
    }
    LaiuePersistBlobRelease(blob);
    blob->data = data;
    blob->size = chunks->size;
    blob->capacity = chunks->size;
    return true;
}

LaiuePersistStatus LaiuePersistReaderReadUpgraded(LaiuePersistReader *reader, uint32_t index,
                                                  uint32_t targetVersion,
                                                  const LaiuePersistMigration *migrations,
                                                  uint32_t migrationCount,
                                                  LaiuePersistBlob *outBlob)
{
    const PersistEntry *entry = PersistReaderEntry(reader, index);
    if (outBlob != NULL)
        memset(outBlob, 0, sizeof(*outBlob));
    if (entry == NULL || outBlob == NULL || (migrations == NULL && migrationCount != 0u))
        return LAIUE_PERSIST_INVALID_ARGUMENT;
    if (entry->schemaVersion > targetVersion)
        return LAIUE_PERSIST_UNSUPPORTED_VERSION;
    if (entry->rawSize > SIZE_MAX)
        return LAIUE_PERSIST_TOO_LARGE;
    uint8_t *data = PlatformAllocate(entry->rawSize == 0u ? 1u : (size_t)entry->rawSize, false);
    if (data == NULL)
        return LAIUE_PERSIST_OUT_OF_MEMORY;
    outBlob->data = data;
    outBlob->size = entry->rawSize;
    outBlob->capacity = entry->rawSize;
    LaiuePersistStatus status = LaiuePersistReaderRead(reader, index, data, entry->rawSize);
    for (uint32_t version = entry->schemaVersion;
         status == LAIUE_PERSIST_OK && version < targetVersion; ++version)
    {
        const LaiuePersistMigration *step = NULL;
        for (uint32_t candidate = 0u; candidate < migrationCount; ++candidate)
            if (migrations[candidate].fromVersion == version &&
                migrations[candidate].migrate != NULL)
            {
                step = &migrations[candidate];
                break;
            }
        if (step == NULL)
        {
            status = LAIUE_PERSIST_MIGRATION_FAILED;
            break;
        }
        PersistChunks chunks = {0};
        LaiuePersistSectionWriter writer;
        PersistWriterInit(&writer, &chunks);
        if (!step->migrate(step->context, version, outBlob->data, outBlob->size, &writer))
            status = LAIUE_PERSIST_MIGRATION_FAILED;
        else if (writer.status != LAIUE_PERSIST_OK)
            status = writer.status;
        else if (!BlobFromChunks(&chunks, outBlob))
            status = LAIUE_PERSIST_OUT_OF_MEMORY;
        PersistChunksRelease(&chunks);
    }
    if (status != LAIUE_PERSIST_OK)
        LaiuePersistBlobRelease(outBlob);
    return status;
}
