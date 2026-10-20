#pragma once

#include "persist/persist.h"
#include "persist/persist_lz.h"

#include "platform/system.h"

/* ---- Раскладка на диске (little-endian) ------------------------------------
 *
 *   [пролог 32 байта][данные секций ...][оглавление][эпилог 40 байт]
 *
 * Пролог пишется первым, оглавление и эпилог — последними, поэтому файл
 * пишется одним последовательным проходом. Читатель берёт пролог с начала
 * и эпилог с конца файла. */

#define PERSIST_FORMAT_VERSION 1u

#define PERSIST_PROLOGUE_SIZE 32u
/* "LAIUESAV" */
#define PERSIST_PROLOGUE_MAGIC_0 UINT32_C(0x5549414C)
#define PERSIST_PROLOGUE_MAGIC_1 UINT32_C(0x56415345)
/*  0 magic[8]
 *  8 u32 formatVersion
 * 12 u32 prologueSize
 * 16 u64 applicationId
 * 24 u32 applicationVersion
 * 28 u32 crc32c байтов 0..27 */

#define PERSIST_EPILOGUE_SIZE 40u
/* "LSVE" */
#define PERSIST_EPILOGUE_MAGIC UINT32_C(0x4556534C)
/*  0 u64 directoryOffset
 *  8 u64 directorySize
 * 16 u64 fileSize
 * 24 u32 sectionCount
 * 28 u32 crc32c оглавления
 * 32 u32 magic
 * 36 u32 crc32c байтов 0..35 */

#define PERSIST_ENTRY_SIZE 128u
/*   0 char name[80], дополнено нулями
 *  80 u64 offset
 *  88 u64 storedSize
 *  96 u64 rawSize
 * 104 u32 schemaVersion
 * 108 u32 flags: биты 0–7 — флаги секции, 8–15 — кодек, 16–31 — ноль
 * 112 u32 crc32c хранимых байтов
 * 116 u32 crc32c исходных байтов
 * 120 u64 tag
 * Записи идут строго по возрастанию имени. */

#define PERSIST_SECTION_FLAG_MASK UINT32_C(0x000000FF)
#define PERSIST_KNOWN_SECTION_FLAGS LAIUE_PERSIST_SECTION_REQUIRED

/* Блок LZ: u32 rawLength (1–BLOCK_SIZE), u32 storedLength; старший бит
 * storedLength означает, что блок хранится несжатым. */
#define PERSIST_BLOCK_HEADER_SIZE 8u
#define PERSIST_BLOCK_UNCOMPRESSED UINT32_C(0x80000000)

#define PERSIST_IO_CHUNK (1024u * 1024u)
/* Самый длинный суффикс пути: ".tmp" либо ".16". */
#define PERSIST_PATH_SUFFIX_CAPACITY 8u
/* Путь сохранения в UTF-8 вместе с завершающим нулём и суффиксом. Больше
 * страницы стека на два пути без CRT не уложить, а длиннее килобайта пути к
 * сохранениям не бывают. */
#define PERSIST_PATH_CAPACITY 1024u

typedef struct PersistEntry
{
    char name[LAIUE_PERSIST_SECTION_NAME_CAPACITY];
    uint64_t offset;
    uint64_t storedSize;
    uint64_t rawSize;
    uint32_t schemaVersion;
    uint32_t flags;
    LaiuePersistCodec codec;
    uint32_t storedCrc;
    uint32_t rawCrc;
    uint64_t tag;
} PersistEntry;

/* Данные секции кусками по размеру блока сжатия: блок — ровно кусок. */
typedef struct PersistChunks
{
    uint8_t **chunks;
    uint32_t count;
    uint32_t capacity;
    uint64_t size;
} PersistChunks;

typedef enum PersistSectionKind
{
    PERSIST_SECTION_OWNED,
    PERSIST_SECTION_BORROWED,
    PERSIST_SECTION_PRODUCED,
    PERSIST_SECTION_COPIED,
} PersistSectionKind;

typedef struct PersistSection
{
    char name[LAIUE_PERSIST_SECTION_NAME_CAPACITY];
    uint32_t schemaVersion;
    uint32_t flags;
    uint64_t tag;
    PersistSectionKind kind;
    PersistChunks chunks;
    const uint8_t *borrowed;
    uint64_t borrowedSize;
    LaiuePersistProducerFn producer;
    void *producerContext;
    LaiuePersistReader *source;
    uint32_t sourceIndex;
} PersistSection;

struct LaiuePersistSectionWriter
{
    PersistChunks *target;
    LaiuePersistDocument *document;
    LaiuePersistStatus status;
    bool open;
};

struct LaiuePersistDocument
{
    uint64_t applicationId;
    uint32_t applicationVersion;
    PersistSection *sections;
    uint32_t count;
    uint32_t capacity;
    LaiuePersistSectionWriter writer;
};

struct LaiuePersistReader
{
    PlatformReadFile file;
    PlatformMutex mutex;
    uint64_t fileSize;
    uint64_t applicationId;
    uint32_t applicationVersion;
    uint32_t formatVersion;
    PersistEntry *entries;
    uint32_t count;
};

/* Общий прогресс заданий; все поля меняются атомарно. */
typedef struct PersistProgressCounters
{
    volatile uint32_t cancel;
    volatile uint32_t sectionsDone;
    volatile uint32_t sectionsTotal;
    volatile int64_t bytesDone;
    volatile int64_t bytesTotal;
} PersistProgressCounters;

bool PersistNameValid(const char *name, uint32_t *outLength);
int PersistNameCompare(const char *first, const char *second);
void PersistStoreU32(uint8_t *bytes, uint32_t value);
void PersistStoreU64(uint8_t *bytes, uint64_t value);
uint32_t PersistLoadU32(const uint8_t *bytes);
uint64_t PersistLoadU64(const uint8_t *bytes);
/* path + suffix в output ёмкостью PERSIST_PATH_CAPACITY. */
bool PersistBuildPath(const char *path, const char *suffix, char *output);
/* Строка ".k" для резервной копии k (1–16). */
void PersistBackupSuffix(uint32_t index, char suffix[PERSIST_PATH_SUFFIX_CAPACITY]);
/* Стабильная сортировка индексов по имени. scratch — count элементов. */
void PersistSortIndices(uint32_t *indices, uint32_t *scratch, uint32_t count,
                        const char *(*nameAt)(const void *context, uint32_t index),
                        const void *context);

void PersistChunksRelease(PersistChunks *chunks);
bool PersistChunksAppend(PersistChunks *chunks, const void *bytes, uint64_t size);
void PersistWriterInit(LaiuePersistSectionWriter *writer, PersistChunks *target);

/* Считывает хранимые байты секции в buffer начиная с внутреннего offset. */
LaiuePersistStatus PersistReaderReadStored(LaiuePersistReader *reader, const PersistEntry *entry,
                                           uint64_t offset, void *buffer, uint32_t count);
const PersistEntry *PersistReaderEntry(const LaiuePersistReader *reader, uint32_t index);

LaiuePersistStatus PersistSaveRun(LaiuePersistDocument *document, const LaiuePersistSaveDesc *desc,
                                  PersistProgressCounters *progress);
LaiuePersistStatus PersistOpenRun(const LaiuePersistOpenDesc *desc, LaiuePersistReader **outReader,
                                  LaiuePersistOpenReport *outReport,
                                  PersistProgressCounters *progress);
