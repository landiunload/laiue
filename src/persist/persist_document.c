#include "persist/persist_internal.h"

#include <string.h>

static bool NameCharacterAllowed(char character)
{
    return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
           (character >= '0' && character <= '9') || character == '.' || character == '_' ||
           character == '-' || character == '/';
}

bool PersistNameValid(const char *name, uint32_t *outLength)
{
    if (name == NULL)
        return false;
    uint32_t length = 0u;
    uint32_t componentStart = 0u;
    for (;;)
    {
        const char character = name[length];
        if (character == '\0' || character == '/')
        {
            const uint32_t componentLength = length - componentStart;
            if (componentLength == 0u)
                return false;
            const char *component = name + componentStart;
            if ((componentLength == 1u && component[0] == '.') ||
                (componentLength == 2u && component[0] == '.' && component[1] == '.'))
                return false;
            if (character == '\0')
                break;
            componentStart = length + 1u;
        }
        else if (!NameCharacterAllowed(character))
            return false;
        if (++length >= LAIUE_PERSIST_SECTION_NAME_CAPACITY)
            return false;
    }
    if (outLength != NULL)
        *outLength = length;
    return true;
}

int PersistNameCompare(const char *first, const char *second)
{
    /* Имена дополнены нулями до полной ёмкости, поэтому побайтовое
     * сравнение массивов совпадает со сравнением строк. */
    return memcmp(first, second, LAIUE_PERSIST_SECTION_NAME_CAPACITY);
}

void PersistStoreU32(uint8_t *bytes, uint32_t value)
{
    for (uint32_t index = 0u; index < 4u; ++index)
        bytes[index] = (uint8_t)(value >> (index * 8u));
}

void PersistStoreU64(uint8_t *bytes, uint64_t value)
{
    for (uint32_t index = 0u; index < 8u; ++index)
        bytes[index] = (uint8_t)(value >> (index * 8u));
}

uint32_t PersistLoadU32(const uint8_t *bytes)
{
    uint32_t value = 0u;
    for (uint32_t index = 0u; index < 4u; ++index)
        value |= (uint32_t)bytes[index] << (index * 8u);
    return value;
}

uint64_t PersistLoadU64(const uint8_t *bytes)
{
    uint64_t value = 0u;
    for (uint32_t index = 0u; index < 8u; ++index)
        value |= (uint64_t)bytes[index] << (index * 8u);
    return value;
}

bool PersistBuildPath(const char *path, const char *suffix, char *output)
{
    if (path == NULL || suffix == NULL || output == NULL)
        return false;
    uint32_t length = 0u;
    while (path[length] != '\0')
    {
        if (length + PERSIST_PATH_SUFFIX_CAPACITY >= PERSIST_PATH_CAPACITY)
            return false;
        output[length] = path[length];
        ++length;
    }
    if (length == 0u)
        return false;
    for (uint32_t index = 0u; suffix[index] != '\0'; ++index)
        output[length++] = suffix[index];
    output[length] = '\0';
    return true;
}

void PersistBackupSuffix(uint32_t index, char suffix[PERSIST_PATH_SUFFIX_CAPACITY])
{
    uint32_t length = 0u;
    suffix[length++] = '.';
    if (index >= 10u)
        suffix[length++] = (char)('0' + index / 10u);
    suffix[length++] = (char)('0' + index % 10u);
    suffix[length] = '\0';
}

void PersistSortIndices(uint32_t *indices, uint32_t *scratch, uint32_t count,
                        const char *(*nameAt)(const void *context, uint32_t index),
                        const void *context)
{
    /* Восходящее слияние: стабильно, O(n log n), без рекурсии. */
    uint32_t *source = indices;
    uint32_t *target = scratch;
    for (uint32_t width = 1u; width<count; width = width> UINT32_MAX / 2u ? count : width * 2u)
    {
        for (uint32_t left = 0u; left < count; left += width * 2u)
        {
            const uint32_t middle = count - left > width ? left + width : count;
            const uint32_t right = count - middle > width ? middle + width : count;
            uint32_t first = left;
            uint32_t second = middle;
            uint32_t output = left;
            while (first < middle && second < right)
            {
                if (PersistNameCompare(nameAt(context, source[second]),
                                       nameAt(context, source[first])) < 0)
                    target[output++] = source[second++];
                else
                    target[output++] = source[first++];
            }
            while (first < middle)
                target[output++] = source[first++];
            while (second < right)
                target[output++] = source[second++];
            if (count - left <= width * 2u)
                break;
        }
        uint32_t *swap = source;
        source = target;
        target = swap;
    }
    if (source != indices)
        memcpy(indices, source, (size_t)count * sizeof(*indices));
}

void PersistChunksRelease(PersistChunks *chunks)
{
    if (chunks == NULL)
        return;
    for (uint32_t index = 0u; index < chunks->count; ++index)
        PlatformFree(chunks->chunks[index]);
    PlatformFree((void *)chunks->chunks);
    memset(chunks, 0, sizeof(*chunks));
}

bool PersistChunksAppend(PersistChunks *chunks, const void *bytes, uint64_t size)
{
    const uint8_t *source = bytes;
    while (size != 0u)
    {
        const uint32_t used = (uint32_t)(chunks->size % LAIUE_PERSIST_LZ_BLOCK_SIZE);
        if (used == 0u && chunks->size / LAIUE_PERSIST_LZ_BLOCK_SIZE == chunks->count)
        {
            if (chunks->count == chunks->capacity)
            {
                if (chunks->capacity > UINT32_MAX / 2u)
                    return false;
                const uint32_t capacity = chunks->capacity == 0u ? 8u : chunks->capacity * 2u;
                uint8_t **grown = (uint8_t **)PlatformReallocate(
                    (void *)chunks->chunks, (size_t)capacity * sizeof(uint8_t *), false);
                if (grown == NULL)
                    return false;
                chunks->chunks = grown;
                chunks->capacity = capacity;
            }
            uint8_t *chunk = PlatformAllocate(LAIUE_PERSIST_LZ_BLOCK_SIZE, false);
            if (chunk == NULL)
                return false;
            chunks->chunks[chunks->count++] = chunk;
        }
        uint32_t part = LAIUE_PERSIST_LZ_BLOCK_SIZE - used;
        if (part > size)
            part = (uint32_t)size;
        memcpy(chunks->chunks[chunks->size / LAIUE_PERSIST_LZ_BLOCK_SIZE] + used, source, part);
        source += part;
        size -= part;
        chunks->size += part;
    }
    return true;
}

void PersistWriterInit(LaiuePersistSectionWriter *writer, PersistChunks *target)
{
    memset(writer, 0, sizeof(*writer));
    writer->target = target;
    writer->status = LAIUE_PERSIST_OK;
    writer->open = true;
}

bool LaiuePersistSectionWrite(LaiuePersistSectionWriter *writer, const void *bytes, uint64_t size)
{
    if (writer == NULL || !writer->open)
        return false;
    if (writer->status != LAIUE_PERSIST_OK)
        return false;
    if (size == 0u)
        return true;
    if (bytes == NULL)
    {
        writer->status = LAIUE_PERSIST_INVALID_ARGUMENT;
        return false;
    }
    if (size > UINT64_MAX - writer->target->size)
    {
        writer->status = LAIUE_PERSIST_TOO_LARGE;
        return false;
    }
    if (!PersistChunksAppend(writer->target, bytes, size))
    {
        writer->status = LAIUE_PERSIST_OUT_OF_MEMORY;
        return false;
    }
    return true;
}

uint64_t LaiuePersistSectionSize(const LaiuePersistSectionWriter *writer)
{
    return writer == NULL || writer->target == NULL ? 0u : writer->target->size;
}

LaiuePersistStatus LaiuePersistSectionEnd(LaiuePersistSectionWriter *writer)
{
    if (writer == NULL || !writer->open || writer->document == NULL)
        return LAIUE_PERSIST_INVALID_ARGUMENT;
    LaiuePersistDocument *document = writer->document;
    const LaiuePersistStatus status = writer->status;
    writer->open = false;
    if (status != LAIUE_PERSIST_OK)
    {
        /* Неудачная секция убирается целиком: документ остаётся прежним. */
        PersistSection *section = &document->sections[document->count - 1u];
        PersistChunksRelease(&section->chunks);
        --document->count;
    }
    return status;
}

static bool SectionDescValid(const LaiuePersistSectionDesc *desc)
{
    return desc != NULL && desc->structSize >= sizeof(LaiuePersistSectionDesc) &&
           PersistNameValid(desc->name, NULL) && (desc->flags & ~PERSIST_KNOWN_SECTION_FLAGS) == 0u;
}

static PersistSection *AppendSection(LaiuePersistDocument *document,
                                     const LaiuePersistSectionDesc *desc, PersistSectionKind kind)
{
    if (document->count >= LAIUE_PERSIST_MAXIMUM_SECTIONS)
        return NULL;
    if (document->count == document->capacity)
    {
        const uint32_t capacity = document->capacity == 0u ? 16u : document->capacity * 2u;
        PersistSection *grown =
            PlatformReallocate(document->sections, (size_t)capacity * sizeof(*grown), false);
        if (grown == NULL)
            return NULL;
        document->sections = grown;
        document->capacity = capacity;
    }
    PersistSection *section = &document->sections[document->count++];
    memset(section, 0, sizeof(*section));
    uint32_t length = 0u;
    (void)PersistNameValid(desc->name, &length);
    memcpy(section->name, desc->name, length);
    section->schemaVersion = desc->schemaVersion;
    section->flags = desc->flags;
    section->tag = desc->tag;
    section->kind = kind;
    return section;
}

LaiuePersistStatus LaiuePersistDocumentCreate(const LaiuePersistDocumentDesc *desc,
                                              LaiuePersistDocument **outDocument)
{
    if (outDocument != NULL)
        *outDocument = NULL;
    if (desc == NULL || outDocument == NULL || desc->structSize < sizeof(*desc))
        return LAIUE_PERSIST_INVALID_ARGUMENT;
    LaiuePersistDocument *document = PlatformAllocate(sizeof(*document), true);
    if (document == NULL)
        return LAIUE_PERSIST_OUT_OF_MEMORY;
    document->applicationId = desc->applicationId;
    document->applicationVersion = desc->applicationVersion;
    *outDocument = document;
    return LAIUE_PERSIST_OK;
}

void LaiuePersistDocumentDestroy(LaiuePersistDocument *document)
{
    if (document == NULL)
        return;
    for (uint32_t index = 0u; index < document->count; ++index)
        PersistChunksRelease(&document->sections[index].chunks);
    PlatformFree(document->sections);
    PlatformFree(document);
}

LaiuePersistStatus LaiuePersistDocumentBeginSection(LaiuePersistDocument *document,
                                                    const LaiuePersistSectionDesc *desc,
                                                    LaiuePersistSectionWriter **outWriter)
{
    if (outWriter != NULL)
        *outWriter = NULL;
    if (document == NULL || outWriter == NULL || !SectionDescValid(desc) || document->writer.open)
        return LAIUE_PERSIST_INVALID_ARGUMENT;
    PersistSection *section = AppendSection(document, desc, PERSIST_SECTION_OWNED);
    if (section == NULL)
        return LAIUE_PERSIST_OUT_OF_MEMORY;
    PersistWriterInit(&document->writer, &section->chunks);
    document->writer.document = document;
    *outWriter = &document->writer;
    return LAIUE_PERSIST_OK;
}

LaiuePersistStatus LaiuePersistDocumentAddBorrowed(LaiuePersistDocument *document,
                                                   const LaiuePersistSectionDesc *desc,
                                                   const void *bytes, uint64_t size)
{
    if (document == NULL || !SectionDescValid(desc) || (bytes == NULL && size != 0u) ||
        document->writer.open)
        return LAIUE_PERSIST_INVALID_ARGUMENT;
    PersistSection *section = AppendSection(document, desc, PERSIST_SECTION_BORROWED);
    if (section == NULL)
        return LAIUE_PERSIST_OUT_OF_MEMORY;
    section->borrowed = bytes;
    section->borrowedSize = size;
    return LAIUE_PERSIST_OK;
}

LaiuePersistStatus LaiuePersistDocumentAddProduced(LaiuePersistDocument *document,
                                                   const LaiuePersistSectionDesc *desc,
                                                   LaiuePersistProducerFn producer, void *context)
{
    if (document == NULL || !SectionDescValid(desc) || producer == NULL || document->writer.open)
        return LAIUE_PERSIST_INVALID_ARGUMENT;
    PersistSection *section = AppendSection(document, desc, PERSIST_SECTION_PRODUCED);
    if (section == NULL)
        return LAIUE_PERSIST_OUT_OF_MEMORY;
    section->producer = producer;
    section->producerContext = context;
    return LAIUE_PERSIST_OK;
}

LaiuePersistStatus LaiuePersistDocumentCopySection(LaiuePersistDocument *document,
                                                   LaiuePersistReader *reader,
                                                   uint32_t sectionIndex)
{
    const PersistEntry *entry = PersistReaderEntry(reader, sectionIndex);
    if (document == NULL || entry == NULL || document->writer.open)
        return LAIUE_PERSIST_INVALID_ARGUMENT;
    const LaiuePersistSectionDesc desc = {
        .structSize = sizeof(desc),
        .name = entry->name,
        .schemaVersion = entry->schemaVersion,
        .flags = entry->flags & PERSIST_KNOWN_SECTION_FLAGS,
        .tag = entry->tag,
    };
    PersistSection *section = AppendSection(document, &desc, PERSIST_SECTION_COPIED);
    if (section == NULL)
        return LAIUE_PERSIST_OUT_OF_MEMORY;
    /* Неизвестные флаги чужой версии переносятся как есть. */
    section->flags = entry->flags;
    section->source = reader;
    section->sourceIndex = sectionIndex;
    return LAIUE_PERSIST_OK;
}

static const char *DocumentNameAt(const void *context, uint32_t index)
{
    return ((const LaiuePersistDocument *)context)->sections[index].name;
}

LaiuePersistStatus LaiuePersistDocumentCopyRemaining(LaiuePersistDocument *document,
                                                     LaiuePersistReader *reader)
{
    if (document == NULL || reader == NULL || document->writer.open)
        return LAIUE_PERSIST_INVALID_ARGUMENT;
    const uint32_t existing = document->count;
    uint32_t *sorted = NULL;
    if (existing != 0u)
    {
        sorted = PlatformAllocate((size_t)existing * 2u * sizeof(*sorted), false);
        if (sorted == NULL)
            return LAIUE_PERSIST_OUT_OF_MEMORY;
        for (uint32_t index = 0u; index < existing; ++index)
            sorted[index] = index;
        PersistSortIndices(sorted, sorted + existing, existing, DocumentNameAt, document);
    }
    LaiuePersistStatus status = LAIUE_PERSIST_OK;
    for (uint32_t index = 0u; index < reader->count && status == LAIUE_PERSIST_OK; ++index)
    {
        const char *name = reader->entries[index].name;
        uint32_t low = 0u;
        uint32_t high = existing;
        bool present = false;
        while (low < high)
        {
            const uint32_t middle = low + (high - low) / 2u;
            const int order = PersistNameCompare(document->sections[sorted[middle]].name, name);
            if (order == 0)
            {
                present = true;
                break;
            }
            if (order < 0)
                low = middle + 1u;
            else
                high = middle;
        }
        if (!present)
            status = LaiuePersistDocumentCopySection(document, reader, index);
    }
    PlatformFree(sorted);
    return status;
}

void LaiuePersistBlobRelease(LaiuePersistBlob *blob)
{
    if (blob == NULL)
        return;
    PlatformFree(blob->data);
    memset(blob, 0, sizeof(*blob));
}

const char *LaiuePersistStatusName(LaiuePersistStatus status)
{
    static const char *const names[] = {
        "ok",
        "invalid argument",
        "out of memory",
        "i/o error",
        "not found",
        "corrupt",
        "unsupported version",
        "wrong application",
        "duplicate section",
        "buffer too small",
        "cancelled",
        "producer failed",
        "migration failed",
        "too large",
        "consumer failed",
    };
    return status < sizeof(names) / sizeof(names[0]) ? names[status] : "unknown";
}
