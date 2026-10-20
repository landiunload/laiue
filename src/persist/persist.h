#pragma once

/* laiue.persist — долговечные сохранения для игр и приложений.
 *
 * Файл сохранения — набор именованных секций. У каждой секции своя версия
 * схемы, флаги, метка приложения и контрольные суммы; секции можно читать по
 * одной, поэтому огромный мир не обязан помещаться в память целиком.
 *
 * Запись: приложение собирает LaiuePersistDocument (быстро, на своём потоке),
 * а сжатие, контрольные суммы и запись на диск идут в фоновом задании.
 * Файл сначала пишется рядом во временный, синхронизируется с носителем и
 * только потом атомарно заменяет прежний; прежние версии уходят в резервные
 * копии path.1 … path.N. Обрыв питания в любой момент оставляет хотя бы одну
 * целую копию, а LaiuePersistOpen сам выбирает самую свежую целую.
 *
 * Совместимость: неизвестную секцию читатель пропускает, а писатель может
 * перенести её в новое сохранение байт в байт (LaiuePersistDocumentCopy*),
 * поэтому выключенный мод не теряет своих данных. Секция с флагом REQUIRED
 * сообщает, что без её понимания сохранение загружать нельзя; решает это
 * приложение. Старые версии схем поднимаются цепочкой миграций.
 *
 * Формат описан в docs/persist.md. Все числа на диске little-endian. */

#include "api.h"
#include "task/task_pool.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef uint32_t LaiuePersistStatus;
#define LAIUE_PERSIST_OK UINT32_C(0)
#define LAIUE_PERSIST_INVALID_ARGUMENT UINT32_C(1)
#define LAIUE_PERSIST_OUT_OF_MEMORY UINT32_C(2)
#define LAIUE_PERSIST_IO_ERROR UINT32_C(3)
#define LAIUE_PERSIST_NOT_FOUND UINT32_C(4)
#define LAIUE_PERSIST_CORRUPT UINT32_C(5)
#define LAIUE_PERSIST_UNSUPPORTED_VERSION UINT32_C(6)
#define LAIUE_PERSIST_WRONG_APPLICATION UINT32_C(7)
#define LAIUE_PERSIST_DUPLICATE_SECTION UINT32_C(8)
#define LAIUE_PERSIST_BUFFER_TOO_SMALL UINT32_C(9)
#define LAIUE_PERSIST_CANCELLED UINT32_C(10)
#define LAIUE_PERSIST_PRODUCER_FAILED UINT32_C(11)
#define LAIUE_PERSIST_MIGRATION_FAILED UINT32_C(12)
#define LAIUE_PERSIST_TOO_LARGE UINT32_C(13)
#define LAIUE_PERSIST_CONSUMER_FAILED UINT32_C(14)

/* Имя секции: 1–79 байт из [A-Za-z0-9._/-], без начального '/', без
 * пустых компонентов и без "." и ".." как компонентов. Пример:
 * "city/region/12/-7". Сравнение побайтовое и чувствительно к регистру. */
#define LAIUE_PERSIST_SECTION_NAME_CAPACITY 80u

/* Приложение не сможет корректно загрузить сохранение без этой секции. */
#define LAIUE_PERSIST_SECTION_REQUIRED UINT32_C(0x00000001)

typedef uint32_t LaiuePersistCodec;
#define LAIUE_PERSIST_CODEC_NONE UINT32_C(0)
/* Собственное LZ-сжатие блоками по 256 КиБ; блоки сжимаются параллельно. */
#define LAIUE_PERSIST_CODEC_LZ UINT32_C(1)

#define LAIUE_PERSIST_MAXIMUM_BACKUPS 16u
#define LAIUE_PERSIST_MAXIMUM_SECTIONS UINT32_C(16777216)

typedef struct LaiuePersistDocument LaiuePersistDocument;
typedef struct LaiuePersistSectionWriter LaiuePersistSectionWriter;
typedef struct LaiuePersistReader LaiuePersistReader;
typedef struct LaiuePersistJob LaiuePersistJob;

/* Произвольный участок памяти, выделенный модулем; освобождается только
 * LaiuePersistBlobRelease. */
typedef struct LaiuePersistBlob
{
    uint8_t *data;
    uint64_t size;
    uint64_t capacity;
} LaiuePersistBlob;

typedef struct LaiuePersistDocumentDesc
{
    uint32_t structSize;
    /* Идентификатор приложения; читатель может требовать совпадения. */
    uint64_t applicationId;
    /* Версия приложения, записавшего файл; движок её не толкует. */
    uint32_t applicationVersion;
} LaiuePersistDocumentDesc;

typedef struct LaiuePersistSectionDesc
{
    uint32_t structSize;
    const char *name;
    uint32_t schemaVersion;
    uint32_t flags;
    /* Метка приложения, например упакованный ключ региона. */
    uint64_t tag;
} LaiuePersistSectionDesc;

/* Вызывается на потоке сохранения. Пишет секцию в writer и возвращает true;
 * false отменяет сохранение со статусом PRODUCER_FAILED. Данные, которые
 * читает producer, приложение обязано не менять до конца сохранения. */
typedef bool (*LaiuePersistProducerFn)(void *context, LaiuePersistSectionWriter *writer);

typedef struct LaiuePersistSaveDesc
{
    uint32_t structSize;
    /* UTF-8 путь итогового файла, не длиннее 1015 байт. Рядом создаются
     * path.tmp и path.1 … path.N. */
    const char *path;
    /* Сколько прежних версий хранить, 0–LAIUE_PERSIST_MAXIMUM_BACKUPS. */
    uint32_t backupCount;
    LaiuePersistCodec codec;
    /* Необязательный исполнитель для параллельного сжатия. Пул, из которого
     * он получен, на время сохранения принадлежит только этому заданию. */
    const LaiueTaskExecutor *executor;
} LaiuePersistSaveDesc;

typedef uint32_t LaiuePersistVerify;
/* Проверяются заголовок, оглавление и границы секций. */
#define LAIUE_PERSIST_VERIFY_STRUCTURE UINT32_C(0)
/* Дополнительно прочитываются все секции и сверяются их контрольные суммы. */
#define LAIUE_PERSIST_VERIFY_FULL UINT32_C(1)

typedef struct LaiuePersistOpenDesc
{
    uint32_t structSize;
    const char *path;
    /* Сколько резервных копий пробовать после основного файла. */
    uint32_t backupCount;
    /* 0 — принимать любое приложение. */
    uint64_t expectedApplicationId;
    LaiuePersistVerify verify;
} LaiuePersistOpenDesc;

typedef struct LaiuePersistOpenReport
{
    uint32_t structSize;
    /* 0 — основной файл, k — path.k. */
    uint32_t chosenCandidate;
    /* Статус каждого кандидата: [0] основной, [k] — path.k. */
    LaiuePersistStatus candidateStatus[LAIUE_PERSIST_MAXIMUM_BACKUPS + 1u];
} LaiuePersistOpenReport;

typedef struct LaiuePersistFileInfo
{
    uint32_t structSize;
    uint32_t formatVersion;
    uint64_t applicationId;
    uint32_t applicationVersion;
    uint32_t sectionCount;
    uint64_t fileSize;
} LaiuePersistFileInfo;

typedef struct LaiuePersistSectionInfo
{
    uint32_t structSize;
    char name[LAIUE_PERSIST_SECTION_NAME_CAPACITY];
    uint32_t schemaVersion;
    uint32_t flags;
    LaiuePersistCodec codec;
    uint64_t tag;
    uint64_t rawSize;
    uint64_t storedSize;
} LaiuePersistSectionInfo;

/* Шаг миграции поднимает схему секции с fromVersion на fromVersion + 1.
 * input действителен только во время вызова; результат пишется в output. */
typedef bool (*LaiuePersistMigrateFn)(void *context, uint32_t fromVersion, const uint8_t *input,
                                      uint64_t inputSize, LaiuePersistSectionWriter *output);

typedef struct LaiuePersistMigration
{
    uint32_t fromVersion;
    LaiuePersistMigrateFn migrate;
    void *context;
} LaiuePersistMigration;

typedef uint32_t LaiuePersistJobState;
#define LAIUE_PERSIST_JOB_RUNNING UINT32_C(0)
#define LAIUE_PERSIST_JOB_SUCCEEDED UINT32_C(1)
#define LAIUE_PERSIST_JOB_FAILED UINT32_C(2)
#define LAIUE_PERSIST_JOB_CANCELLED UINT32_C(3)

typedef struct LaiuePersistProgress
{
    uint32_t structSize;
    LaiuePersistJobState state;
    /* Итоговый статус; до завершения — LAIUE_PERSIST_OK. */
    LaiuePersistStatus status;
    uint32_t sectionsDone;
    uint32_t sectionsTotal;
    uint64_t bytesDone;
    /* Оценка; у сохранения с producer-секциями растёт по мере их записи. */
    uint64_t bytesTotal;
} LaiuePersistProgress;

/* Вызывается на потоке загрузки по одной секции в порядке оглавления
 * (по возрастанию имени). bytes действительны только во время вызова.
 * false прерывает загрузку со статусом CONSUMER_FAILED. */
typedef bool (*LaiuePersistConsumerFn)(void *context, const LaiuePersistSectionInfo *section,
                                       const uint8_t *bytes, uint64_t size);
/* Необязательный фильтр: false — секция пропускается без чтения. */
typedef bool (*LaiuePersistFilterFn)(void *context, const LaiuePersistSectionInfo *section);

typedef struct LaiuePersistLoadDesc
{
    uint32_t structSize;
    LaiuePersistOpenDesc open;
    LaiuePersistFilterFn filter;
    LaiuePersistConsumerFn consumer;
    void *context;
} LaiuePersistLoadDesc;

/* ---- Документ ---------------------------------------------------------
 * Документ принадлежит одному потоку. Он не читает файлов и не делает I/O. */

LAIUE_PERSIST_API LaiuePersistStatus LaiuePersistDocumentCreate(
    const LaiuePersistDocumentDesc *desc, LaiuePersistDocument **outDocument);
LAIUE_PERSIST_API void LaiuePersistDocumentDestroy(LaiuePersistDocument *document);

/* Начинает секцию, данные которой копируются в документ. До
 * LaiuePersistSectionEnd другая секция этого документа не начинается. */
LAIUE_PERSIST_API LaiuePersistStatus LaiuePersistDocumentBeginSection(
    LaiuePersistDocument *document, const LaiuePersistSectionDesc *desc,
    LaiuePersistSectionWriter **outWriter);
/* Без копирования: bytes должны жить и не меняться до конца сохранения. */
LAIUE_PERSIST_API LaiuePersistStatus
LaiuePersistDocumentAddBorrowed(LaiuePersistDocument *document, const LaiuePersistSectionDesc *desc,
                                const void *bytes, uint64_t size);
/* Секция, которую producer построит уже на потоке сохранения. */
LAIUE_PERSIST_API LaiuePersistStatus
LaiuePersistDocumentAddProduced(LaiuePersistDocument *document, const LaiuePersistSectionDesc *desc,
                                LaiuePersistProducerFn producer, void *context);
/* Переносит секцию открытого сохранения байт в байт, без распаковки.
 * reader должен оставаться открытым до конца сохранения. */
LAIUE_PERSIST_API LaiuePersistStatus LaiuePersistDocumentCopySection(LaiuePersistDocument *document,
                                                                     LaiuePersistReader *reader,
                                                                     uint32_t sectionIndex);
/* Переносит все секции reader, имён которых ещё нет в документе. Так
 * сохранение пишет заново только изменённое, а остальное копирует. */
LAIUE_PERSIST_API LaiuePersistStatus
LaiuePersistDocumentCopyRemaining(LaiuePersistDocument *document, LaiuePersistReader *reader);

/* ---- Запись секции ------------------------------------------------------
 * Ошибка записи «залипает»: следующие вызовы ничего не делают, а
 * LaiuePersistSectionEnd и сохранение вернут её статус. */

LAIUE_PERSIST_API bool LaiuePersistSectionWrite(LaiuePersistSectionWriter *writer,
                                                const void *bytes, uint64_t size);
LAIUE_PERSIST_API uint64_t LaiuePersistSectionSize(const LaiuePersistSectionWriter *writer);
LAIUE_PERSIST_API LaiuePersistStatus LaiuePersistSectionEnd(LaiuePersistSectionWriter *writer);

static inline bool LaiuePersistWriteU8(LaiuePersistSectionWriter *writer, uint8_t value)
{
    return LaiuePersistSectionWrite(writer, &value, 1u);
}

static inline bool LaiuePersistWriteU16(LaiuePersistSectionWriter *writer, uint16_t value)
{
    const uint8_t bytes[2] = {(uint8_t)value, (uint8_t)(value >> 8u)};
    return LaiuePersistSectionWrite(writer, bytes, sizeof(bytes));
}

static inline bool LaiuePersistWriteU32(LaiuePersistSectionWriter *writer, uint32_t value)
{
    uint8_t bytes[4];
    for (uint32_t index = 0u; index < 4u; ++index)
        bytes[index] = (uint8_t)(value >> (index * 8u));
    return LaiuePersistSectionWrite(writer, bytes, sizeof(bytes));
}

static inline bool LaiuePersistWriteU64(LaiuePersistSectionWriter *writer, uint64_t value)
{
    uint8_t bytes[8];
    for (uint32_t index = 0u; index < 8u; ++index)
        bytes[index] = (uint8_t)(value >> (index * 8u));
    return LaiuePersistSectionWrite(writer, bytes, sizeof(bytes));
}

static inline bool LaiuePersistWriteI32(LaiuePersistSectionWriter *writer, int32_t value)
{
    return LaiuePersistWriteU32(writer, (uint32_t)value);
}

static inline bool LaiuePersistWriteI64(LaiuePersistSectionWriter *writer, int64_t value)
{
    return LaiuePersistWriteU64(writer, (uint64_t)value);
}

/* Битовый образ IEEE 754; NaN сохраняет свою полезную нагрузку. */
static inline bool LaiuePersistWriteF32(LaiuePersistSectionWriter *writer, float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return LaiuePersistWriteU32(writer, bits);
}

static inline bool LaiuePersistWriteF64(LaiuePersistSectionWriter *writer, double value)
{
    uint64_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return LaiuePersistWriteU64(writer, bits);
}

/* LEB128: 1–10 байт, мелкие значения занимают байт. */
static inline bool LaiuePersistWriteVarU64(LaiuePersistSectionWriter *writer, uint64_t value)
{
    uint8_t bytes[10];
    uint32_t count = 0u;
    do
    {
        uint8_t byte = (uint8_t)(value & 0x7fu);
        value >>= 7u;
        if (value != 0u)
            byte |= 0x80u;
        bytes[count++] = byte;
    } while (value != 0u);
    return LaiuePersistSectionWrite(writer, bytes, count);
}

/* Zigzag поверх LEB128: малые по модулю отрицательные тоже коротки. */
static inline bool LaiuePersistWriteVarI64(LaiuePersistSectionWriter *writer, int64_t value)
{
    const uint64_t bits = (uint64_t)value;
    return LaiuePersistWriteVarU64(writer, (bits << 1u) ^ (0u - (bits >> 63u)));
}

/* Длина в LEB128, затем байты без завершающего нуля. */
static inline bool LaiuePersistWriteBytes(LaiuePersistSectionWriter *writer, const void *bytes,
                                          uint64_t size)
{
    return LaiuePersistWriteVarU64(writer, size) && LaiuePersistSectionWrite(writer, bytes, size);
}

/* ---- Чтение секции -------------------------------------------------------
 * Курсор только читает чужую память. Ошибка (выход за границу, слишком
 * длинный varint) «залипает»: дальше возвращаются нули, а failed остаётся
 * true, поэтому разбор проверяется один раз в конце. */

typedef struct LaiuePersistCursor
{
    const uint8_t *data;
    uint64_t size;
    uint64_t offset;
    bool failed;
} LaiuePersistCursor;

static inline LaiuePersistCursor LaiuePersistCursorMake(const void *data, uint64_t size)
{
    LaiuePersistCursor cursor = {(const uint8_t *)data, data == NULL ? 0u : size, 0u, false};
    return cursor;
}

static inline const uint8_t *LaiuePersistCursorTake(LaiuePersistCursor *cursor, uint64_t size)
{
    if (cursor->failed || size > cursor->size - cursor->offset)
    {
        cursor->failed = true;
        return NULL;
    }
    const uint8_t *result = cursor->data + cursor->offset;
    cursor->offset += size;
    return result;
}

static inline bool LaiuePersistCursorRead(LaiuePersistCursor *cursor, void *output, uint64_t size)
{
    const uint8_t *bytes = LaiuePersistCursorTake(cursor, size);
    if (bytes == NULL)
    {
        if (output != NULL && size != 0u)
            memset(output, 0, (size_t)size);
        return false;
    }
    if (size != 0u)
        memcpy(output, bytes, (size_t)size);
    return true;
}

static inline uint64_t LaiuePersistReadLittle(LaiuePersistCursor *cursor, uint32_t byteCount)
{
    const uint8_t *bytes = LaiuePersistCursorTake(cursor, byteCount);
    uint64_t value = 0u;
    if (bytes != NULL)
        for (uint32_t index = 0u; index < byteCount; ++index)
            value |= (uint64_t)bytes[index] << (index * 8u);
    return value;
}

static inline uint8_t LaiuePersistReadU8(LaiuePersistCursor *cursor)
{
    return (uint8_t)LaiuePersistReadLittle(cursor, 1u);
}

static inline uint16_t LaiuePersistReadU16(LaiuePersistCursor *cursor)
{
    return (uint16_t)LaiuePersistReadLittle(cursor, 2u);
}

static inline uint32_t LaiuePersistReadU32(LaiuePersistCursor *cursor)
{
    return (uint32_t)LaiuePersistReadLittle(cursor, 4u);
}

static inline uint64_t LaiuePersistReadU64(LaiuePersistCursor *cursor)
{
    return LaiuePersistReadLittle(cursor, 8u);
}

static inline int32_t LaiuePersistReadI32(LaiuePersistCursor *cursor)
{
    return (int32_t)LaiuePersistReadU32(cursor);
}

static inline int64_t LaiuePersistReadI64(LaiuePersistCursor *cursor)
{
    return (int64_t)LaiuePersistReadU64(cursor);
}

static inline float LaiuePersistReadF32(LaiuePersistCursor *cursor)
{
    const uint32_t bits = LaiuePersistReadU32(cursor);
    float value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static inline double LaiuePersistReadF64(LaiuePersistCursor *cursor)
{
    const uint64_t bits = LaiuePersistReadU64(cursor);
    double value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static inline uint64_t LaiuePersistReadVarU64(LaiuePersistCursor *cursor)
{
    uint64_t value = 0u;
    for (uint32_t shift = 0u; shift < 70u; shift += 7u)
    {
        const uint8_t *byte = LaiuePersistCursorTake(cursor, 1u);
        if (byte == NULL)
            return 0u;
        const uint64_t part = (uint64_t)(*byte & 0x7fu);
        /* Десятый байт несёт только старший бит; лишние биты — порча. */
        if (shift == 63u && part > 1u)
            break;
        value |= part << shift;
        if ((*byte & 0x80u) == 0u)
            return value;
    }
    cursor->failed = true;
    return 0u;
}

static inline int64_t LaiuePersistReadVarI64(LaiuePersistCursor *cursor)
{
    const uint64_t bits = LaiuePersistReadVarU64(cursor);
    return (int64_t)((bits >> 1u) ^ (0u - (bits & 1u)));
}

/* Возвращает указатель внутрь секции; *outSize — длина. */
static inline const uint8_t *LaiuePersistReadBytes(LaiuePersistCursor *cursor, uint64_t *outSize)
{
    const uint64_t size = LaiuePersistReadVarU64(cursor);
    const uint8_t *bytes = LaiuePersistCursorTake(cursor, size);
    if (outSize != NULL)
        *outSize = bytes == NULL ? 0u : size;
    return bytes;
}

/* Курсор дочитан ровно до конца и без ошибок. */
static inline bool LaiuePersistCursorFinished(const LaiuePersistCursor *cursor)
{
    return !cursor->failed && cursor->offset == cursor->size;
}

/* ---- Сохранение ---------------------------------------------------------- */

/* Синхронно на вызывающем потоке. Документ после вызова можно уничтожить. */
LAIUE_PERSIST_API LaiuePersistStatus LaiuePersistSave(LaiuePersistDocument *document,
                                                      const LaiuePersistSaveDesc *desc);
/* В фоне: документ переходит заданию и уничтожается им, в том числе при
 * отказе запуска. Пути из desc копируются. */
LAIUE_PERSIST_API LaiuePersistStatus LaiuePersistSaveStart(LaiuePersistDocument *document,
                                                           const LaiuePersistSaveDesc *desc,
                                                           LaiuePersistJob **outJob);

/* ---- Чтение файла ---------------------------------------------------------
 * Читатель потокобезопасен: секции можно читать параллельно. */

LAIUE_PERSIST_API LaiuePersistStatus LaiuePersistOpen(const LaiuePersistOpenDesc *desc,
                                                      LaiuePersistReader **outReader,
                                                      LaiuePersistOpenReport *outReport);
LAIUE_PERSIST_API void LaiuePersistClose(LaiuePersistReader *reader);
LAIUE_PERSIST_API bool LaiuePersistReaderGetInfo(const LaiuePersistReader *reader,
                                                 LaiuePersistFileInfo *outInfo);
LAIUE_PERSIST_API bool LaiuePersistReaderGetSection(const LaiuePersistReader *reader,
                                                    uint32_t index,
                                                    LaiuePersistSectionInfo *outInfo);
/* Двоичный поиск по оглавлению. */
LAIUE_PERSIST_API LaiuePersistStatus LaiuePersistReaderFind(const LaiuePersistReader *reader,
                                                            const char *name, uint32_t *outIndex);
/* Распаковывает секцию в буфер вызывающего ёмкостью не меньше rawSize и
 * сверяет обе контрольные суммы. */
LAIUE_PERSIST_API LaiuePersistStatus LaiuePersistReaderRead(LaiuePersistReader *reader,
                                                            uint32_t index, void *buffer,
                                                            uint64_t capacity);
/* Читает секцию и поднимает её схему до targetVersion шагами migrations.
 * Секция уже нужной версии возвращается без миграций; более новая —
 * UNSUPPORTED_VERSION. Результат освобождается LaiuePersistBlobRelease. */
LAIUE_PERSIST_API LaiuePersistStatus LaiuePersistReaderReadUpgraded(
    LaiuePersistReader *reader, uint32_t index, uint32_t targetVersion,
    const LaiuePersistMigration *migrations, uint32_t migrationCount, LaiuePersistBlob *outBlob);
LAIUE_PERSIST_API void LaiuePersistBlobRelease(LaiuePersistBlob *blob);

/* Загрузка в фоне: открытие (с выбором целой копии) и чтение секций идут на
 * отдельном потоке, вызывающий поток остаётся свободен для экрана загрузки. */
LAIUE_PERSIST_API LaiuePersistStatus LaiuePersistLoadStart(const LaiuePersistLoadDesc *desc,
                                                           LaiuePersistJob **outJob);

/* ---- Фоновые задания -------------------------------------------------------
 * Poll и Cancel можно вызывать с любого потока. Destroy дожидается конца. */

LAIUE_PERSIST_API bool LaiuePersistJobPoll(LaiuePersistJob *job, LaiuePersistProgress *outProgress);
LAIUE_PERSIST_API void LaiuePersistJobCancel(LaiuePersistJob *job);
LAIUE_PERSIST_API LaiuePersistStatus LaiuePersistJobWait(LaiuePersistJob *job);
LAIUE_PERSIST_API void LaiuePersistJobDestroy(LaiuePersistJob *job);

/* Короткое английское имя статуса для журналов. */
LAIUE_PERSIST_API const char *LaiuePersistStatusName(LaiuePersistStatus status);
