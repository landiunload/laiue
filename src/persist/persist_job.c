#include "persist/persist_internal.h"

#include <string.h>

typedef enum PersistJobKind
{
    PERSIST_JOB_SAVE,
    PERSIST_JOB_LOAD,
} PersistJobKind;

struct LaiuePersistJob
{
    PersistJobKind kind;
    PlatformThread thread;
    bool joined;
    volatile uint32_t state;
    volatile uint32_t status;
    PersistProgressCounters progress;
    char path[PERSIST_PATH_CAPACITY];
    LaiuePersistDocument *document;
    LaiuePersistSaveDesc save;
    LaiueTaskExecutor executor;
    LaiuePersistLoadDesc load;
};

static void Finish(LaiuePersistJob *job, LaiuePersistStatus status)
{
    PlatformAtomicStoreU32Release(&job->status, status);
    const uint32_t state = status == LAIUE_PERSIST_OK          ? LAIUE_PERSIST_JOB_SUCCEEDED
                           : status == LAIUE_PERSIST_CANCELLED ? LAIUE_PERSIST_JOB_CANCELLED
                                                               : LAIUE_PERSIST_JOB_FAILED;
    PlatformAtomicStoreU32Release(&job->state, state);
}

static uint32_t SaveThread(void *context)
{
    LaiuePersistJob *job = context;
    const LaiuePersistStatus status = PersistSaveRun(job->document, &job->save, &job->progress);
    LaiuePersistDocumentDestroy(job->document);
    job->document = NULL;
    Finish(job, status);
    return 0u;
}

static LaiuePersistStatus LoadSections(LaiuePersistJob *job, LaiuePersistReader *reader)
{
    const LaiuePersistLoadDesc *desc = &job->load;
    uint8_t *buffer = NULL;
    uint64_t capacity = 0u;
    LaiuePersistStatus status = LAIUE_PERSIST_OK;
    uint64_t total = 0u;
    for (uint32_t index = 0u; index < reader->count; ++index)
        total += reader->entries[index].storedSize;
    (void)PlatformAtomicAddI64(&job->progress.bytesTotal, (int64_t)total);
    PlatformAtomicStoreU32Release(&job->progress.sectionsTotal, reader->count);
    for (uint32_t index = 0u; index < reader->count && status == LAIUE_PERSIST_OK; ++index)
    {
        if (PlatformAtomicLoadU32Acquire(&job->progress.cancel) != 0u)
        {
            status = LAIUE_PERSIST_CANCELLED;
            break;
        }
        const PersistEntry *entry = &reader->entries[index];
        LaiuePersistSectionInfo info;
        memset(&info, 0, sizeof(info));
        info.structSize = sizeof(info);
        (void)LaiuePersistReaderGetSection(reader, index, &info);
        if (desc->filter == NULL || desc->filter(desc->context, &info))
        {
            if (entry->rawSize > SIZE_MAX)
            {
                status = LAIUE_PERSIST_TOO_LARGE;
                break;
            }
            if (entry->rawSize > capacity)
            {
                uint8_t *grown = PlatformReallocate(buffer, (size_t)entry->rawSize, false);
                if (grown == NULL)
                {
                    status = LAIUE_PERSIST_OUT_OF_MEMORY;
                    break;
                }
                buffer = grown;
                capacity = entry->rawSize;
            }
            status = LaiuePersistReaderRead(reader, index, buffer, capacity);
            if (status == LAIUE_PERSIST_OK &&
                !desc->consumer(desc->context, &info, buffer, entry->rawSize))
                status = LAIUE_PERSIST_CONSUMER_FAILED;
        }
        (void)PlatformAtomicAddI64(&job->progress.bytesDone, (int64_t)entry->storedSize);
        (void)PlatformAtomicIncrementU32(&job->progress.sectionsDone);
    }
    PlatformFree(buffer);
    return status;
}

static uint32_t LoadThread(void *context)
{
    LaiuePersistJob *job = context;
    LaiuePersistReader *reader = NULL;
    LaiuePersistStatus status = PersistOpenRun(&job->load.open, &reader, NULL, &job->progress);
    if (status == LAIUE_PERSIST_OK)
    {
        /* Полная проверка уже учла байты; чтение считает их заново. */
        (void)PlatformAtomicAddI64(&job->progress.bytesDone,
                                   -PlatformAtomicLoadI64(&job->progress.bytesDone));
        status = LoadSections(job, reader);
        LaiuePersistClose(reader);
    }
    Finish(job, status);
    return 0u;
}

static bool CopyPath(char *output, const char *path)
{
    return PersistBuildPath(path, "", output);
}

static LaiuePersistJob *CreateJob(PersistJobKind kind)
{
    LaiuePersistJob *job = PlatformAllocate(sizeof(*job), true);
    if (job != NULL)
    {
        job->kind = kind;
        job->state = LAIUE_PERSIST_JOB_RUNNING;
    }
    return job;
}

LaiuePersistStatus LaiuePersistSaveStart(LaiuePersistDocument *document,
                                         const LaiuePersistSaveDesc *desc, LaiuePersistJob **outJob)
{
    if (outJob != NULL)
        *outJob = NULL;
    if (document == NULL)
        return LAIUE_PERSIST_INVALID_ARGUMENT;
    if (desc == NULL || outJob == NULL || desc->structSize < sizeof(*desc) || desc->path == NULL ||
        document->writer.open ||
        (desc->executor != NULL && desc->executor->structSize < sizeof(LaiueTaskExecutor)))
    {
        LaiuePersistDocumentDestroy(document);
        return LAIUE_PERSIST_INVALID_ARGUMENT;
    }
    LaiuePersistJob *job = CreateJob(PERSIST_JOB_SAVE);
    if (job == NULL)
    {
        LaiuePersistDocumentDestroy(document);
        return LAIUE_PERSIST_OUT_OF_MEMORY;
    }
    if (!CopyPath(job->path, desc->path))
    {
        PlatformFree(job);
        LaiuePersistDocumentDestroy(document);
        return LAIUE_PERSIST_INVALID_ARGUMENT;
    }
    job->document = document;
    job->save = *desc;
    job->save.structSize = sizeof(job->save);
    job->save.path = job->path;
    if (desc->executor != NULL)
    {
        job->executor = *desc->executor;
        job->executor.structSize = sizeof(job->executor);
        job->save.executor = &job->executor;
    }
    if (!PlatformThreadStart(&job->thread, SaveThread, job))
    {
        PlatformFree(job);
        LaiuePersistDocumentDestroy(document);
        return LAIUE_PERSIST_OUT_OF_MEMORY;
    }
    *outJob = job;
    return LAIUE_PERSIST_OK;
}

LaiuePersistStatus LaiuePersistLoadStart(const LaiuePersistLoadDesc *desc, LaiuePersistJob **outJob)
{
    if (outJob != NULL)
        *outJob = NULL;
    if (desc == NULL || outJob == NULL || desc->structSize < sizeof(*desc) ||
        desc->consumer == NULL || desc->open.structSize < sizeof(desc->open) ||
        desc->open.path == NULL || desc->open.backupCount > LAIUE_PERSIST_MAXIMUM_BACKUPS ||
        desc->open.verify > LAIUE_PERSIST_VERIFY_FULL)
        return LAIUE_PERSIST_INVALID_ARGUMENT;
    LaiuePersistJob *job = CreateJob(PERSIST_JOB_LOAD);
    if (job == NULL)
        return LAIUE_PERSIST_OUT_OF_MEMORY;
    if (!CopyPath(job->path, desc->open.path))
    {
        PlatformFree(job);
        return LAIUE_PERSIST_INVALID_ARGUMENT;
    }
    job->load = *desc;
    job->load.structSize = sizeof(job->load);
    job->load.open.structSize = sizeof(job->load.open);
    job->load.open.path = job->path;
    if (!PlatformThreadStart(&job->thread, LoadThread, job))
    {
        PlatformFree(job);
        return LAIUE_PERSIST_OUT_OF_MEMORY;
    }
    *outJob = job;
    return LAIUE_PERSIST_OK;
}

bool LaiuePersistJobPoll(LaiuePersistJob *job, LaiuePersistProgress *outProgress)
{
    if (job == NULL || outProgress == NULL || outProgress->structSize < sizeof(*outProgress))
        return false;
    outProgress->state = PlatformAtomicLoadU32Acquire(&job->state);
    outProgress->status = PlatformAtomicLoadU32Acquire(&job->status);
    outProgress->sectionsDone = PlatformAtomicLoadU32Acquire(&job->progress.sectionsDone);
    outProgress->sectionsTotal = PlatformAtomicLoadU32Acquire(&job->progress.sectionsTotal);
    const int64_t done = PlatformAtomicLoadI64(&job->progress.bytesDone);
    const int64_t total = PlatformAtomicLoadI64(&job->progress.bytesTotal);
    outProgress->bytesDone = done < 0 ? 0u : (uint64_t)done;
    outProgress->bytesTotal = total < 0 ? 0u : (uint64_t)total;
    if (outProgress->bytesTotal < outProgress->bytesDone)
        outProgress->bytesTotal = outProgress->bytesDone;
    return true;
}

void LaiuePersistJobCancel(LaiuePersistJob *job)
{
    if (job != NULL)
        PlatformAtomicStoreU32Release(&job->progress.cancel, 1u);
}

LaiuePersistStatus LaiuePersistJobWait(LaiuePersistJob *job)
{
    if (job == NULL)
        return LAIUE_PERSIST_INVALID_ARGUMENT;
    if (!job->joined)
    {
        PlatformThreadJoin(&job->thread);
        job->joined = true;
    }
    return PlatformAtomicLoadU32Acquire(&job->status);
}

void LaiuePersistJobDestroy(LaiuePersistJob *job)
{
    if (job == NULL)
        return;
    (void)LaiuePersistJobWait(job);
    PlatformFree(job);
}
