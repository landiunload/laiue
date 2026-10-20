#include "persist/persist_crc.h"
#include "persist/persist_internal.h"

#include <string.h>

/* Столько блоков сжимается за один параллельный проход: 8 МиБ исходных
 * данных и примерно столько же под результат. */
#define SAVE_BATCH_BLOCKS 32u

typedef struct SaveBlock
{
    const uint8_t *raw;
    uint32_t rawLength;
    uint32_t storedLength; /* 0 — сжатие не помогло, блок пишется как есть. */
} SaveBlock;

typedef struct SaveState
{
    LaiuePersistDocument *document;
    const LaiuePersistSaveDesc *desc;
    PersistProgressCounters *progress;
    PlatformWriteFile file;
    uint64_t written;
    PersistEntry *entries;
    uint32_t *order;
    SaveBlock blocks[SAVE_BATCH_BLOCKS];
    uint8_t *compressed; /* SAVE_BATCH_BLOCKS × BOUND(BLOCK_SIZE) */
    uint32_t *scratch;   /* SAVE_BATCH_BLOCKS × таблица кодировщика */
    uint8_t *copyBuffer; /* PERSIST_IO_CHUNK */
} SaveState;

static bool Cancelled(const SaveState *state)
{
    return state->progress != NULL && PlatformAtomicLoadU32Acquire(&state->progress->cancel) != 0u;
}

static void AddBytesDone(SaveState *state, uint64_t bytes)
{
    if (state->progress != NULL)
        (void)PlatformAtomicAddI64(&state->progress->bytesDone, (int64_t)bytes);
}

static void AddBytesTotal(SaveState *state, uint64_t bytes)
{
    if (state->progress != NULL)
        (void)PlatformAtomicAddI64(&state->progress->bytesTotal, (int64_t)bytes);
}

static LaiuePersistStatus WriteBytes(SaveState *state, const void *bytes, uint64_t size)
{
    const uint8_t *source = bytes;
    while (size != 0u)
    {
        const uint32_t part = size > PERSIST_IO_CHUNK ? PERSIST_IO_CHUNK : (uint32_t)size;
        if (!PlatformFileWrite(&state->file, source, part))
            return LAIUE_PERSIST_IO_ERROR;
        source += part;
        size -= part;
        state->written += part;
    }
    return LAIUE_PERSIST_OK;
}

static uint64_t SectionRawSize(const PersistSection *section)
{
    switch (section->kind)
    {
    case PERSIST_SECTION_BORROWED:
        return section->borrowedSize;
    case PERSIST_SECTION_COPIED:
        return PersistReaderEntry(section->source, section->sourceIndex)->rawSize;
    default:
        return section->chunks.size;
    }
}

static const uint8_t *SectionBlock(const PersistSection *section, uint64_t blockIndex,
                                   uint32_t *outLength)
{
    const uint64_t size = SectionRawSize(section);
    const uint64_t offset = blockIndex * LAIUE_PERSIST_LZ_BLOCK_SIZE;
    *outLength = size - offset < LAIUE_PERSIST_LZ_BLOCK_SIZE ? (uint32_t)(size - offset)
                                                             : LAIUE_PERSIST_LZ_BLOCK_SIZE;
    if (section->kind == PERSIST_SECTION_BORROWED)
        return section->borrowed + offset;
    return section->chunks.chunks[blockIndex];
}

static void CompressRange(void *context, uint32_t begin, uint32_t end)
{
    SaveState *state = context;
    for (uint32_t index = begin; index < end; ++index)
    {
        SaveBlock *block = &state->blocks[index];
        const uint32_t capacity = LAIUE_PERSIST_LZ_BOUND(LAIUE_PERSIST_LZ_BLOCK_SIZE);
        block->storedLength = LaiuePersistLzCompress(
            block->raw, block->rawLength, state->compressed + (size_t)index * capacity, capacity,
            state->scratch + (size_t)index * (LAIUE_PERSIST_LZ_SCRATCH_BYTES / sizeof(uint32_t)));
    }
}

static LaiuePersistStatus WriteGenerated(SaveState *state, const PersistSection *section,
                                         PersistEntry *entry)
{
    const uint64_t rawSize = SectionRawSize(section);
    entry->rawSize = rawSize;
    entry->codec = rawSize == 0u ? LAIUE_PERSIST_CODEC_NONE : state->desc->codec;
    uint32_t rawCrc = 0u;
    uint32_t storedCrc = 0u;
    const uint64_t blockCount =
        rawSize / LAIUE_PERSIST_LZ_BLOCK_SIZE + (rawSize % LAIUE_PERSIST_LZ_BLOCK_SIZE != 0u);
    for (uint64_t first = 0u; first < blockCount; first += SAVE_BATCH_BLOCKS)
    {
        if (Cancelled(state))
            return LAIUE_PERSIST_CANCELLED;
        const uint32_t batch = blockCount - first < SAVE_BATCH_BLOCKS
                                   ? (uint32_t)(blockCount - first)
                                   : SAVE_BATCH_BLOCKS;
        for (uint32_t index = 0u; index < batch; ++index)
        {
            SaveBlock *block = &state->blocks[index];
            block->raw = SectionBlock(section, first + index, &block->rawLength);
            block->storedLength = 0u;
        }
        if (entry->codec == LAIUE_PERSIST_CODEC_LZ)
        {
            const LaiueTaskExecutor *executor = state->desc->executor;
            if (executor != NULL && executor->run != NULL && batch > 1u)
                executor->run(executor->context, batch, 1u, CompressRange, state);
            else
                CompressRange(state, 0u, batch);
        }
        for (uint32_t index = 0u; index < batch; ++index)
        {
            const SaveBlock *block = &state->blocks[index];
            rawCrc = LaiuePersistCrc32c(rawCrc, block->raw, block->rawLength);
            LaiuePersistStatus status;
            if (entry->codec == LAIUE_PERSIST_CODEC_LZ)
            {
                const bool compressed = block->storedLength != 0u;
                const uint8_t *payload =
                    compressed
                        ? state->compressed +
                              (size_t)index * LAIUE_PERSIST_LZ_BOUND(LAIUE_PERSIST_LZ_BLOCK_SIZE)
                        : block->raw;
                const uint32_t length = compressed ? block->storedLength : block->rawLength;
                uint8_t header[PERSIST_BLOCK_HEADER_SIZE];
                PersistStoreU32(header, block->rawLength);
                PersistStoreU32(header + 4u,
                                compressed ? length : length | PERSIST_BLOCK_UNCOMPRESSED);
                storedCrc = LaiuePersistCrc32c(storedCrc, header, sizeof(header));
                storedCrc = LaiuePersistCrc32c(storedCrc, payload, length);
                status = WriteBytes(state, header, sizeof(header));
                if (status == LAIUE_PERSIST_OK)
                    status = WriteBytes(state, payload, length);
            }
            else
                status = WriteBytes(state, block->raw, block->rawLength);
            if (status != LAIUE_PERSIST_OK)
                return status;
            AddBytesDone(state, block->rawLength);
        }
    }
    entry->rawCrc = rawCrc;
    entry->storedCrc = entry->codec == LAIUE_PERSIST_CODEC_LZ ? storedCrc : rawCrc;
    return LAIUE_PERSIST_OK;
}

static LaiuePersistStatus WriteCopied(SaveState *state, const PersistSection *section,
                                      PersistEntry *entry)
{
    const PersistEntry *source = PersistReaderEntry(section->source, section->sourceIndex);
    entry->rawSize = source->rawSize;
    entry->codec = source->codec;
    entry->rawCrc = source->rawCrc;
    entry->storedCrc = source->storedCrc;
    uint32_t crc = 0u;
    for (uint64_t offset = 0u; offset < source->storedSize;)
    {
        if (Cancelled(state))
            return LAIUE_PERSIST_CANCELLED;
        uint64_t part = source->storedSize - offset;
        if (part > PERSIST_IO_CHUNK)
            part = PERSIST_IO_CHUNK;
        LaiuePersistStatus status = PersistReaderReadStored(section->source, source, offset,
                                                            state->copyBuffer, (uint32_t)part);
        if (status != LAIUE_PERSIST_OK)
            return status;
        crc = LaiuePersistCrc32c(crc, state->copyBuffer, part);
        status = WriteBytes(state, state->copyBuffer, part);
        if (status != LAIUE_PERSIST_OK)
            return status;
        offset += part;
        AddBytesDone(state, part);
    }
    /* Повреждённая секция прежнего файла не переносится в новый. */
    return crc == source->storedCrc ? LAIUE_PERSIST_OK : LAIUE_PERSIST_CORRUPT;
}

static const char *SectionNameAt(const void *context, uint32_t index)
{
    return ((const LaiuePersistDocument *)context)->sections[index].name;
}

static LaiuePersistStatus WriteDirectoryAndEpilogue(SaveState *state)
{
    const uint32_t count = state->document->count;
    const uint64_t directoryOffset = state->written;
    uint8_t record[PERSIST_ENTRY_SIZE];
    uint32_t crc = 0u;
    for (uint32_t index = 0u; index < count; ++index)
    {
        const PersistEntry *entry = &state->entries[index];
        memset(record, 0, sizeof(record));
        memcpy(record, entry->name, LAIUE_PERSIST_SECTION_NAME_CAPACITY);
        PersistStoreU64(record + 80u, entry->offset);
        PersistStoreU64(record + 88u, entry->storedSize);
        PersistStoreU64(record + 96u, entry->rawSize);
        PersistStoreU32(record + 104u, entry->schemaVersion);
        PersistStoreU32(record + 108u,
                        (entry->flags & PERSIST_SECTION_FLAG_MASK) | (entry->codec << 8u));
        PersistStoreU32(record + 112u, entry->storedCrc);
        PersistStoreU32(record + 116u, entry->rawCrc);
        PersistStoreU64(record + 120u, entry->tag);
        crc = LaiuePersistCrc32c(crc, record, sizeof(record));
        const LaiuePersistStatus status = WriteBytes(state, record, sizeof(record));
        if (status != LAIUE_PERSIST_OK)
            return status;
    }
    uint8_t epilogue[PERSIST_EPILOGUE_SIZE];
    const uint64_t directorySize = (uint64_t)count * PERSIST_ENTRY_SIZE;
    PersistStoreU64(epilogue, directoryOffset);
    PersistStoreU64(epilogue + 8u, directorySize);
    PersistStoreU64(epilogue + 16u, state->written + PERSIST_EPILOGUE_SIZE);
    PersistStoreU32(epilogue + 24u, count);
    PersistStoreU32(epilogue + 28u, crc);
    PersistStoreU32(epilogue + 32u, PERSIST_EPILOGUE_MAGIC);
    PersistStoreU32(epilogue + 36u, LaiuePersistCrc32c(0u, epilogue, 36u));
    return WriteBytes(state, epilogue, sizeof(epilogue));
}

static LaiuePersistStatus WriteFileBody(SaveState *state)
{
    LaiuePersistDocument *document = state->document;
    uint8_t prologue[PERSIST_PROLOGUE_SIZE];
    PersistStoreU32(prologue, PERSIST_PROLOGUE_MAGIC_0);
    PersistStoreU32(prologue + 4u, PERSIST_PROLOGUE_MAGIC_1);
    PersistStoreU32(prologue + 8u, PERSIST_FORMAT_VERSION);
    PersistStoreU32(prologue + 12u, PERSIST_PROLOGUE_SIZE);
    PersistStoreU64(prologue + 16u, document->applicationId);
    PersistStoreU32(prologue + 24u, document->applicationVersion);
    PersistStoreU32(prologue + 28u, LaiuePersistCrc32c(0u, prologue, 28u));
    LaiuePersistStatus status = WriteBytes(state, prologue, sizeof(prologue));
    for (uint32_t position = 0u; position < document->count && status == LAIUE_PERSIST_OK;
         ++position)
    {
        const PersistSection *section = &document->sections[state->order[position]];
        PersistEntry *entry = &state->entries[position];
        memset(entry, 0, sizeof(*entry));
        memcpy(entry->name, section->name, sizeof(entry->name));
        entry->schemaVersion = section->schemaVersion;
        entry->flags = section->flags;
        entry->tag = section->tag;
        entry->offset = state->written;
        status = section->kind == PERSIST_SECTION_COPIED ? WriteCopied(state, section, entry)
                                                         : WriteGenerated(state, section, entry);
        entry->storedSize = state->written - entry->offset;
        if (state->progress != NULL)
            (void)PlatformAtomicIncrementU32(&state->progress->sectionsDone);
    }
    if (status == LAIUE_PERSIST_OK)
        status = WriteDirectoryAndEpilogue(state);
    return status;
}

/* Пути сохранения живут в куче: без CRT нет __chkstk, а кадр больше
 * страницы стека получается, как только компилятор встроит соседние
 * функции друг в друга. */
typedef struct SavePaths
{
    char temporary[PERSIST_PATH_CAPACITY];
    char from[PERSIST_PATH_CAPACITY];
    char to[PERSIST_PATH_CAPACITY];
} SavePaths;

/* Сдвигает path → path.1 → … → path.N; самая старая копия удаляется. */
static LaiuePersistStatus RotateBackups(const char *path, uint32_t backupCount, SavePaths *paths)
{
    char *from = paths->from;
    char *to = paths->to;
    char suffix[PERSIST_PATH_SUFFIX_CAPACITY];
    PersistBackupSuffix(backupCount, suffix);
    if (!PersistBuildPath(path, suffix, to) || !PlatformFileDeleteUtf8(to))
        return LAIUE_PERSIST_IO_ERROR;
    for (uint32_t index = backupCount; index >= 1u; --index)
    {
        suffix[0] = '\0';
        if (index > 1u)
            PersistBackupSuffix(index - 1u, suffix);
        char target[PERSIST_PATH_SUFFIX_CAPACITY];
        PersistBackupSuffix(index, target);
        if (!PersistBuildPath(path, suffix, from) || !PersistBuildPath(path, target, to))
            return LAIUE_PERSIST_IO_ERROR;
        PlatformPathInformation information;
        if (!PlatformGetPathInformationUtf8(from, &information))
            return LAIUE_PERSIST_IO_ERROR;
        if (information.exists && !PlatformFileReplaceDurable(from, to))
            return LAIUE_PERSIST_IO_ERROR;
    }
    return LAIUE_PERSIST_OK;
}

static LaiuePersistStatus ProduceSections(SaveState *state)
{
    LaiuePersistDocument *document = state->document;
    for (uint32_t index = 0u; index < document->count; ++index)
    {
        PersistSection *section = &document->sections[index];
        if (section->kind != PERSIST_SECTION_PRODUCED || section->chunks.count != 0u)
            continue;
        if (Cancelled(state))
            return LAIUE_PERSIST_CANCELLED;
        LaiuePersistSectionWriter writer;
        PersistWriterInit(&writer, &section->chunks);
        const bool produced = section->producer(section->producerContext, &writer);
        if (!produced)
            return LAIUE_PERSIST_PRODUCER_FAILED;
        if (writer.status != LAIUE_PERSIST_OK)
            return writer.status;
        AddBytesTotal(state, section->chunks.size);
    }
    return LAIUE_PERSIST_OK;
}

LaiuePersistStatus PersistSaveRun(LaiuePersistDocument *document, const LaiuePersistSaveDesc *desc,
                                  PersistProgressCounters *progress)
{
    if (document == NULL || desc == NULL || desc->structSize < sizeof(*desc) ||
        desc->path == NULL || desc->backupCount > LAIUE_PERSIST_MAXIMUM_BACKUPS ||
        desc->codec > LAIUE_PERSIST_CODEC_LZ || document->writer.open)
        return LAIUE_PERSIST_INVALID_ARGUMENT;
    if (desc->executor != NULL && desc->executor->structSize < sizeof(LaiueTaskExecutor))
        return LAIUE_PERSIST_INVALID_ARGUMENT;
    SavePaths *paths = PlatformAllocate(sizeof(*paths), false);
    if (paths == NULL)
        return LAIUE_PERSIST_OUT_OF_MEMORY;
    char *temporary = paths->temporary;
    char longestSuffix[PERSIST_PATH_SUFFIX_CAPACITY];
    PersistBackupSuffix(LAIUE_PERSIST_MAXIMUM_BACKUPS, longestSuffix);
    /* Самая длинная резервная копия должна поместиться уже сейчас, а не
     * отказать после записи всего файла. */
    if (!PersistBuildPath(desc->path, ".tmp", temporary) ||
        !PersistBuildPath(desc->path, longestSuffix, paths->from))
    {
        PlatformFree(paths);
        return LAIUE_PERSIST_INVALID_ARGUMENT;
    }

    SaveState state;
    memset(&state, 0, sizeof(state));
    state.document = document;
    state.desc = desc;
    state.progress = progress;
    const uint32_t count = document->count;
    if (progress != NULL)
    {
        PlatformAtomicStoreU32Release(&progress->sectionsTotal, count);
        uint64_t known = 0u;
        for (uint32_t index = 0u; index < count; ++index)
            known += SectionRawSize(&document->sections[index]);
        AddBytesTotal(&state, known);
    }

    LaiuePersistStatus status = ProduceSections(&state);
    if (status != LAIUE_PERSIST_OK)
        goto cleanup;

    const size_t orderBytes = (size_t)(count == 0u ? 1u : count) * sizeof(uint32_t);
    state.order = PlatformAllocate(orderBytes * 2u, false);
    state.entries =
        PlatformAllocate((size_t)(count == 0u ? 1u : count) * sizeof(PersistEntry), false);
    state.copyBuffer = PlatformAllocate(PERSIST_IO_CHUNK, false);
    if (desc->codec == LAIUE_PERSIST_CODEC_LZ)
    {
        state.compressed = PlatformAllocate(
            (size_t)SAVE_BATCH_BLOCKS * LAIUE_PERSIST_LZ_BOUND(LAIUE_PERSIST_LZ_BLOCK_SIZE), false);
        state.scratch =
            PlatformAllocate((size_t)SAVE_BATCH_BLOCKS * LAIUE_PERSIST_LZ_SCRATCH_BYTES, false);
    }
    if (state.order == NULL || state.entries == NULL || state.copyBuffer == NULL ||
        (desc->codec == LAIUE_PERSIST_CODEC_LZ &&
         (state.compressed == NULL || state.scratch == NULL)))
    {
        status = LAIUE_PERSIST_OUT_OF_MEMORY;
        goto cleanup;
    }
    for (uint32_t index = 0u; index < count; ++index)
        state.order[index] = index;
    PersistSortIndices(state.order, state.order + (count == 0u ? 1u : count), count, SectionNameAt,
                       document);
    for (uint32_t index = 1u; index < count; ++index)
        if (PersistNameCompare(document->sections[state.order[index - 1u]].name,
                               document->sections[state.order[index]].name) == 0)
        {
            status = LAIUE_PERSIST_DUPLICATE_SECTION;
            goto cleanup;
        }

    if (!PlatformFileCreateWrite(temporary, &state.file))
    {
        status = LAIUE_PERSIST_IO_ERROR;
        goto cleanup;
    }
    status = WriteFileBody(&state);
    if (status == LAIUE_PERSIST_OK && !PlatformFileFlush(&state.file))
        status = LAIUE_PERSIST_IO_ERROR;
    PlatformFileCloseWrite(&state.file);
    if (status == LAIUE_PERSIST_OK && Cancelled(&state))
        status = LAIUE_PERSIST_CANCELLED;
    /* С этого места отмена уже не действует: замена идёт до конца. */
    if (status == LAIUE_PERSIST_OK && desc->backupCount != 0u)
        status = RotateBackups(desc->path, desc->backupCount, paths);
    if (status == LAIUE_PERSIST_OK && !PlatformFileReplaceDurable(temporary, desc->path))
        status = LAIUE_PERSIST_IO_ERROR;
    if (status != LAIUE_PERSIST_OK)
        (void)PlatformFileDeleteUtf8(temporary);

cleanup:
    PlatformFree(paths);
    PlatformFree(state.order);
    PlatformFree(state.entries);
    PlatformFree(state.copyBuffer);
    PlatformFree(state.compressed);
    PlatformFree(state.scratch);
    return status;
}

LaiuePersistStatus LaiuePersistSave(LaiuePersistDocument *document,
                                    const LaiuePersistSaveDesc *desc)
{
    return PersistSaveRun(document, desc, NULL);
}
