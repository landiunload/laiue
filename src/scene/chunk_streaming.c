#include "scene/chunk_streaming.h"
#include "scene/math.h"
#include "world/world.h"
#include "render/renderer.h"
#include "mesh/chunk_mesher.h"
#include "platform/system.h"

#include <string.h>


#define MAX_WORKER_THREADS 4

// Бюджет загрузок на GPU за один кадр: сглаживает волну готовых мешей
// при пересечении границы чанка (иначе — разовый фриз кадра).
#define MESH_UPLOADS_PER_FRAME 4
#define MESH_UPLOAD_BUDGET_MILLISECONDS 2.0
#define CHUNK_MESH_BUILD_FAILED UINT32_MAX

static int64_t ChunkCoordinateFromBlock(int64_t block)
{
    int64_t chunk = block / CHUNK_SIZE;
    return block % CHUNK_SIZE < 0 ? chunk - 1 : chunk;
}

typedef enum ChunkEntryState
{
    CHUNK_ENTRY_EMPTY = 0,
    CHUNK_ENTRY_PENDING,
    CHUNK_ENTRY_READY,
} ChunkEntryState;

// PENDING не гасит отрисовку: mesh (если есть) — последняя готовая
// геометрия, она рисуется, пока рабочий поток строит замену.
typedef struct ChunkEntry
{
    int64_t x;
    int64_t y;
    int64_t z;
    RendererMesh* mesh;
    uint32_t revision;      // растёт при инвалидации: устаревшие результаты отбрасываются
    uint32_t drawSlotPlusOne; // 0 — меша нет, иначе позиция в плотном drawItems + 1
    ChunkEntryState state;
    bool requestQueued;     // есть ли в очереди заявка текущей ревизии
} ChunkEntry;

typedef struct ChunkRequest
{
    int64_t x;
    int64_t y;
    int64_t z;
    uint32_t revision;
    uint32_t centerEpoch;
} ChunkRequest;

typedef struct ChunkMeshResult
{
    int64_t x;
    int64_t y;
    int64_t z;
    ChunkQuad* quads;
    uint32_t revision;
    uint32_t quadCount;
} ChunkMeshResult;

typedef struct DrawItem
{
    float distanceSquared;
    uint32_t entryIndex;
} DrawItem;

struct ChunkStreaming
{
    World* world;
    Renderer* renderer;
    int32_t viewRadius;

    bool hasCenter;
    int64_t centerX;
    int64_t centerY;
    int64_t centerZ;

    // Кеш мешей: открытая адресация, таблица принадлежит главному потоку.
    ChunkEntry* entries;
    ChunkEntry* spareEntries;
    uint32_t capacity;

    // Плотный список записей с мешами. Он же хранит кешированный порядок
    // от ближних к дальним: полную hash-таблицу Draw не обходит.
    DrawItem* drawItems;
    uint32_t drawItemCount;
    int64_t drawRenderOriginBlock[3];
    bool hasDrawRenderOrigin;
    bool drawOrderDirty;

    // true тогда и только тогда, когда возможна PENDING-запись без заявки.
    // Полный retry-скан таблицы выполняется лишь в таком случае.
    bool hasUnqueuedPending;

    // Кольцевые очереди под общим замком. Гарантия отсутствия потерь:
    // unfinishedWork (заявки + в работе + результаты) не превышает
    // ёмкость, поэтому очередь результатов переполниться не может.
    ChunkRequest* requests;
    uint32_t requestHead;
    uint32_t requestCount;
    ChunkMeshResult* results;
    uint32_t resultHead;
    uint32_t resultCount;
    uint32_t queueCapacity;
    uint32_t unfinishedWork;

    PlatformMutex queueLock;
    PlatformConditionVariable workAvailable;
    PlatformThread workerThreads[MAX_WORKER_THREADS];
    uint32_t workerThreadCount;
    uint32_t desiredWorkerThreadCount;
    uint32_t pausedWorkerCount;
    bool pauseRequested;
    bool shutdownRequested;

    volatile int64_t queuedRequests;
    volatile int64_t completedBuilds;
    volatile int64_t cancelledBuilds;
    volatile int64_t discardedBuilds;
    volatile int64_t uploadedMeshes;
    // Время построения копится в микросекундах: монотонные часы платформы
    // выдают секунды с плавающей точкой, а счётчик обязан быть целым,
    // чтобы складываться атомарно из нескольких потоков.
    volatile int64_t totalBuildMicroseconds;
    uint32_t peakUnfinishedWork;
    volatile uint32_t centerEpoch;
};

static void AddMeshToDrawList(ChunkStreaming* streaming, ChunkEntry* entry)
{
    if (entry->drawSlotPlusOne != 0)
    {
        return;
    }

    uint32_t slot = streaming->drawItemCount++;
    streaming->drawItems[slot].entryIndex = (uint32_t)(entry - streaming->entries);
    entry->drawSlotPlusOne = slot + 1u;
    streaming->drawOrderDirty = true;
}

static void RemoveMeshFromDrawList(ChunkStreaming* streaming, ChunkEntry* entry)
{
    if (entry->drawSlotPlusOne == 0)
    {
        return;
    }

    uint32_t slot = entry->drawSlotPlusOne - 1u;
    uint32_t lastSlot = --streaming->drawItemCount;
    if (slot != lastSlot)
    {
        streaming->drawItems[slot] = streaming->drawItems[lastSlot];
        ChunkEntry* moved = &streaming->entries[streaming->drawItems[slot].entryIndex];
        moved->drawSlotPlusOne = slot + 1u;
    }
    entry->drawSlotPlusOne = 0;
    streaming->drawOrderDirty = true;
}

static void ResetDrawList(ChunkStreaming* streaming)
{
    streaming->drawItemCount = 0;
    streaming->drawOrderDirty = true;
}

static void SwapDrawItems(DrawItem* left, DrawItem* right)
{
    DrawItem temporary = *left;
    *left = *right;
    *right = temporary;
}

static void SiftDrawItemsDown(DrawItem* items, uint32_t root, uint32_t count)
{
    for (;;)
    {
        uint32_t leftChild = root * 2u + 1u;
        if (leftChild >= count)
        {
            return;
        }

        uint32_t largest = leftChild;
        uint32_t rightChild = leftChild + 1u;
        if (rightChild < count
            && items[rightChild].distanceSquared > items[leftChild].distanceSquared)
        {
            largest = rightChild;
        }

        if (items[root].distanceSquared >= items[largest].distanceSquared)
        {
            return;
        }

        SwapDrawItems(&items[root], &items[largest]);
        root = largest;
    }
}

static void SortDrawItemsFrontToBack(DrawItem* items, uint32_t count)
{
    if (count < 2)
    {
        return;
    }

    for (uint32_t start = count / 2u; start > 0; --start)
    {
        SiftDrawItemsDown(items, start - 1u, count);
    }

    for (uint32_t end = count; end > 1; --end)
    {
        SwapDrawItems(&items[0], &items[end - 1u]);
        SiftDrawItemsDown(items, 0, end - 1u);
    }
}

static ChunkEntry* FindEntry(const ChunkStreaming* streaming, int64_t x, int64_t y, int64_t z)
{
    uint32_t mask = streaming->capacity - 1;
    uint32_t index = WorldHashChunkCoordinate(x, y, z) & mask;

    for (uint32_t probe = 0; probe < streaming->capacity; ++probe)
    {
        ChunkEntry* entry = &streaming->entries[index];
        if (entry->state == CHUNK_ENTRY_EMPTY)
        {
            return NULL;
        }
        if (entry->x == x && entry->y == y && entry->z == z)
        {
            return entry;
        }
        index = (index + 1) & mask;
    }

    return NULL;
}

static ChunkEntry* InsertEntry(ChunkStreaming* streaming, int64_t x, int64_t y, int64_t z)
{
    uint32_t mask = streaming->capacity - 1;
    uint32_t index = WorldHashChunkCoordinate(x, y, z) & mask;

    while (streaming->entries[index].state != CHUNK_ENTRY_EMPTY)
    {
        index = (index + 1) & mask;
    }

    ChunkEntry* entry = &streaming->entries[index];
    entry->x = x;
    entry->y = y;
    entry->z = z;
    entry->mesh = NULL;
    entry->revision = 0;
    entry->drawSlotPlusOne = 0;
    entry->requestQueued = false;
    return entry;
}

static bool IsInsideRadius(const ChunkStreaming* streaming, int64_t x, int64_t y, int64_t z, int64_t radius)
{
    int64_t deltaX = x - streaming->centerX;
    int64_t deltaY = y - streaming->centerY;
    int64_t deltaZ = z - streaming->centerZ;
    if (deltaX < 0) deltaX = -deltaX;
    if (deltaY < 0) deltaY = -deltaY;
    if (deltaZ < 0) deltaZ = -deltaZ;
    return deltaX <= radius && deltaY <= radius && deltaZ <= radius;
}

// Ставит заявку текущей ревизии записи; false — очередь занята,
// повторная попытка произойдёт в ChunkStreamingPump.
static bool TryEnqueueRequest(ChunkStreaming* streaming, ChunkEntry* entry)
{
    bool enqueued = false;

    PlatformMutexLock(&streaming->queueLock);
    if (streaming->unfinishedWork < streaming->queueCapacity)
    {
        uint32_t queueMask = streaming->queueCapacity - 1;
        ChunkRequest* request = &streaming->requests[(streaming->requestHead + streaming->requestCount) & queueMask];
        request->x = entry->x;
        request->y = entry->y;
        request->z = entry->z;
        request->revision = entry->revision;
        request->centerEpoch = streaming->centerEpoch;
        streaming->requestCount++;
        streaming->unfinishedWork++;
        if (streaming->unfinishedWork > streaming->peakUnfinishedWork)
        {
            streaming->peakUnfinishedWork = streaming->unfinishedWork;
        }
        enqueued = true;
    }
    PlatformMutexUnlock(&streaming->queueLock);

    if (enqueued)
    {
        PlatformAtomicIncrementI64(&streaming->queuedRequests);
        PlatformConditionVariableWakeOne(&streaming->workAvailable);
    }

    entry->requestQueued = enqueued;
    if (!enqueued)
    {
        streaming->hasUnqueuedPending = true;
    }
    return enqueued;
}

static uint32_t WorkerThreadProcedure(void* parameter)
{
    ChunkStreaming* streaming = parameter;

    ChunkMesherScratch* scratch = ChunkMesherScratchCreate();
    if (scratch == NULL)
    {
        return 1;
    }

    bool reportedPaused = false;

    for (;;)
    {
        PlatformMutexLock(&streaming->queueLock);
        while (!streaming->shutdownRequested
            && (streaming->pauseRequested || streaming->requestCount == 0))
        {
            if (streaming->pauseRequested && !reportedPaused)
            {
                reportedPaused = true;
                streaming->pausedWorkerCount++;
                PlatformConditionVariableWakeAll(&streaming->workAvailable);
            }
            PlatformConditionVariableWait(&streaming->workAvailable, &streaming->queueLock);
        }
        if (streaming->shutdownRequested)
        {
            PlatformMutexUnlock(&streaming->queueLock);
            ChunkMesherScratchDestroy(scratch);
            return 0;
        }

        reportedPaused = false;

        uint32_t queueMask = streaming->queueCapacity - 1;
        ChunkRequest request = streaming->requests[streaming->requestHead & queueMask];
        streaming->requestHead++;
        streaming->requestCount--;
        PlatformMutexUnlock(&streaming->queueLock);

        // Тяжёлая работа — без замка.
        bool cancelled =
            request.centerEpoch != PlatformAtomicLoadU32Acquire(&streaming->centerEpoch);
        double buildStart = PlatformMonotonicSeconds();
        ChunkMeshResult result = { .x = request.x, .y = request.y, .z = request.z, .revision = request.revision };
        if (cancelled || !BuildChunkMesh(streaming->world, scratch,
            request.x, request.y, request.z, &result.quads, &result.quadCount))
        {
            result.quadCount = CHUNK_MESH_BUILD_FAILED;
        }
        double buildEnd = PlatformMonotonicSeconds();
        PlatformAtomicAddI64(&streaming->totalBuildMicroseconds,
            (int64_t)((buildEnd - buildStart) * 1000000.0));
        PlatformAtomicIncrementI64(&streaming->completedBuilds);
        if (cancelled) PlatformAtomicIncrementI64(&streaming->cancelledBuilds);

        // Очередь результатов переполниться не может: unfinishedWork
        // ограничен её ёмкостью.
        PlatformMutexLock(&streaming->queueLock);
        streaming->results[(streaming->resultHead + streaming->resultCount) & queueMask] = result;
        streaming->resultCount++;
        PlatformMutexUnlock(&streaming->queueLock);
    }
}

static bool StartWorkerThreads(ChunkStreaming* streaming)
{
    if (streaming->workerThreadCount != 0) return true;
    streaming->shutdownRequested = false;

    for (uint32_t index = 0;
         index < streaming->desiredWorkerThreadCount; ++index)
    {
        PlatformThread thread;
        if (PlatformThreadStart(&thread, WorkerThreadProcedure, streaming))
        {
            streaming->workerThreads[streaming->workerThreadCount++] = thread;
        }
    }
    return streaming->workerThreadCount != 0;
}

static bool StopWorkerThreads(ChunkStreaming* streaming)
{
    if (streaming->workerThreadCount == 0) return true;

    PlatformMutexLock(&streaming->queueLock);
    streaming->shutdownRequested = true;
    streaming->pauseRequested = false;
    streaming->pausedWorkerCount = 0;
    PlatformConditionVariableWakeAll(&streaming->workAvailable);
    PlatformMutexUnlock(&streaming->queueLock);

    for (uint32_t index = 0;
         index < streaming->workerThreadCount; ++index)
    {
        PlatformThreadJoin(&streaming->workerThreads[index]);
    }
    streaming->workerThreadCount = 0;
    streaming->shutdownRequested = false;
    return true;
}

static void ResumeWorkerThreads(ChunkStreaming* streaming)
{
    PlatformMutexLock(&streaming->queueLock);
    streaming->pausedWorkerCount = 0;
    streaming->pauseRequested = false;
    PlatformConditionVariableWakeAll(&streaming->workAvailable);
    PlatformMutexUnlock(&streaming->queueLock);
}

bool ChunkStreamingPause(ChunkStreaming* streaming)
{
    if (streaming->workerThreadCount == 0) return false;

    PlatformMutexLock(&streaming->queueLock);
    streaming->pauseRequested = true;
    PlatformConditionVariableWakeAll(&streaming->workAvailable);
    while (streaming->pausedWorkerCount < streaming->workerThreadCount)
    {
        PlatformConditionVariableWait(&streaming->workAvailable, &streaming->queueLock);
    }

    // Все рабочие потоки стоят на condition variable и больше не читают World.
    uint32_t queueMask = streaming->queueCapacity - 1;
    for (uint32_t index = 0; index < streaming->resultCount; ++index)
    {
        ChunkMeshResult* result = &streaming->results[
            (streaming->resultHead + index) & queueMask];
        if (result->quads != NULL)
        {
            PlatformFree(result->quads);
            result->quads = NULL;
        }
    }

    streaming->requestHead = 0;
    streaming->requestCount = 0;
    streaming->resultHead = 0;
    streaming->resultCount = 0;
    streaming->unfinishedWork = 0;
    streaming->hasUnqueuedPending = false;

    PlatformMutexUnlock(&streaming->queueLock);

    for (uint32_t index = 0; index < streaming->capacity; ++index)
    {
        ChunkEntry* entry = &streaming->entries[index];
        entry->requestQueued = false;
        if (entry->state == CHUNK_ENTRY_PENDING && entry->mesh != NULL)
        {
            entry->state = CHUNK_ENTRY_READY;
        }
        else if (entry->state == CHUNK_ENTRY_PENDING)
        {
            streaming->hasUnqueuedPending = true;
        }
    }
    return true;
}

static bool TrySubtractInt64(
    int64_t value, int64_t difference, int64_t* outValue)
{
    if (difference > 0 && value < INT64_MIN + difference) return false;
    if (difference < 0 && value > INT64_MAX + difference) return false;
    *outValue = value - difference;
    return true;
}

static void QueueMissingChunks(
    ChunkStreaming* streaming, int64_t chunkX, int64_t chunkY, int64_t chunkZ)
{
    for (int64_t shell = 0; shell <= streaming->viewRadius; ++shell)
    {
        for (int64_t deltaZ = -shell; deltaZ <= shell; ++deltaZ)
        {
            for (int64_t deltaY = -shell; deltaY <= shell; ++deltaY)
            {
                for (int64_t deltaX = -shell; deltaX <= shell; ++deltaX)
                {
                    int64_t absoluteX = deltaX < 0 ? -deltaX : deltaX;
                    int64_t absoluteY = deltaY < 0 ? -deltaY : deltaY;
                    int64_t absoluteZ = deltaZ < 0 ? -deltaZ : deltaZ;
                    int64_t chebyshev = absoluteX > absoluteY ? absoluteX : absoluteY;
                    if (absoluteZ > chebyshev) chebyshev = absoluteZ;
                    if (chebyshev != shell) continue;

                    int64_t x = chunkX + deltaX;
                    int64_t y = chunkY + deltaY;
                    int64_t z = chunkZ + deltaZ;
                    if (FindEntry(streaming, x, y, z) != NULL) continue;

                    ChunkEntry* entry = InsertEntry(streaming, x, y, z);
                    entry->state = CHUNK_ENTRY_PENDING;
                    TryEnqueueRequest(streaming, entry);
                }
            }
        }
    }
}

bool ChunkStreamingResumeAfterOriginChange(ChunkStreaming* streaming,
    bool originDeltaFits,
    int64_t chunkOriginDeltaX, int64_t chunkOriginDeltaY, int64_t chunkOriginDeltaZ,
    int64_t newCenterX, int64_t newCenterY, int64_t newCenterZ)
{
    memset(streaming->spareEntries, 0,
        (size_t)streaming->capacity * sizeof(ChunkEntry));

    ChunkEntry* previousEntries = streaming->entries;
    streaming->entries = streaming->spareEntries;
    streaming->spareEntries = previousEntries;
    streaming->hasCenter = true;
    streaming->centerX = newCenterX;
    streaming->centerY = newCenterY;
    streaming->centerZ = newCenterZ;
    streaming->hasUnqueuedPending = false;
    ResetDrawList(streaming);

    for (uint32_t index = 0; index < streaming->capacity; ++index)
    {
        ChunkEntry* previous = &previousEntries[index];
        if (previous->mesh == NULL) continue;

        int64_t x = 0;
        int64_t y = 0;
        int64_t z = 0;
        bool keep = originDeltaFits
            && TrySubtractInt64(previous->x, chunkOriginDeltaX, &x)
            && TrySubtractInt64(previous->y, chunkOriginDeltaY, &y)
            && TrySubtractInt64(previous->z, chunkOriginDeltaZ, &z)
            && IsInsideRadius(streaming, x, y, z,
                (int64_t)streaming->viewRadius + 1);

        if (keep)
        {
            ChunkEntry* moved = InsertEntry(streaming, x, y, z);
            moved->mesh = previous->mesh;
            moved->state = CHUNK_ENTRY_READY;
            AddMeshToDrawList(streaming, moved);
            previous->mesh = NULL;
            previous->drawSlotPlusOne = 0;
        }
        else
        {
            RendererDestroyMesh(streaming->renderer, previous->mesh);
            previous->mesh = NULL;
            previous->drawSlotPlusOne = 0;
        }
    }

    QueueMissingChunks(streaming, newCenterX, newCenterY, newCenterZ);
    ResumeWorkerThreads(streaming);
    return true;
}

ChunkStreaming* ChunkStreamingCreate(World* world, Renderer* renderer, int32_t viewRadiusChunks)
{
    ChunkStreaming* streaming = PlatformAllocate(sizeof(*streaming), true);
    if (streaming == NULL)
    {
        return NULL;
    }

    streaming->world = world;
    streaming->renderer = renderer;
    streaming->viewRadius = viewRadiusChunks;

    // Гистерезис держит максимум куб радиуса + 1. Загрузка около 54%
    // достаточна для быстрой открытой адресации без лишнего удвоения памяти.
    uint32_t diameter = (uint32_t)(viewRadiusChunks * 2 + 3);
    uint32_t volume = diameter * diameter * diameter;
    uint32_t minimumCapacity = volume + volume / 2u;
    uint32_t capacity = 1;
    while (capacity < minimumCapacity)
    {
        capacity <<= 1;
    }

    // Очередям достаточно вместить весь активный куб радиуса viewRadius.
    // Hash-таблица больше из-за гистерезиса, но переносить этот запас в две
    // очереди нет смысла: переполнение всё равно корректно retry-ится.
    uint32_t activeDiameter = (uint32_t)(viewRadiusChunks * 2 + 1);
    uint32_t activeVolume = activeDiameter * activeDiameter * activeDiameter;
    uint32_t queueCapacity = 1;
    while (queueCapacity < activeVolume)
    {
        queueCapacity <<= 1;
    }

    streaming->capacity = capacity;
    streaming->queueCapacity = queueCapacity;
    streaming->entries = PlatformAllocate((size_t)capacity * sizeof(ChunkEntry), true);
    streaming->spareEntries = PlatformAllocate((size_t)capacity * sizeof(ChunkEntry), true);
    streaming->drawItems = PlatformAllocate((size_t)volume * sizeof(DrawItem), false);
    streaming->requests = PlatformAllocate((size_t)queueCapacity * sizeof(ChunkRequest), false);
    streaming->results = PlatformAllocate((size_t)queueCapacity * sizeof(ChunkMeshResult), false);

    if (!PlatformMutexInitialize(&streaming->queueLock)
        || !PlatformConditionVariableInitialize(&streaming->workAvailable))
    {
        ChunkStreamingDestroy(streaming);
        return NULL;
    }

    if (streaming->entries == NULL || streaming->spareEntries == NULL
        || streaming->drawItems == NULL
        || streaming->requests == NULL || streaming->results == NULL)
    {
        ChunkStreamingDestroy(streaming);
        return NULL;
    }

    // Пул потоков мешинга: масштабируется по ядрам процессора.
    uint32_t processorCount = PlatformLogicalProcessorCount();
    streaming->desiredWorkerThreadCount =
        processorCount > 2 ? processorCount - 2 : 1;
    if (streaming->desiredWorkerThreadCount > MAX_WORKER_THREADS)
    {
        streaming->desiredWorkerThreadCount = MAX_WORKER_THREADS;
    }

    if (!StartWorkerThreads(streaming))
    {
        ChunkStreamingDestroy(streaming);
        return NULL;
    }

    return streaming;
}

void ChunkStreamingDestroy(ChunkStreaming* streaming)
{
    if (streaming == NULL)
    {
        return;
    }

    StopWorkerThreads(streaming);

    // Остаточные результаты: освободить CPU-массивы.
    if (streaming->results != NULL)
    {
        uint32_t queueMask = streaming->queueCapacity - 1;
        for (uint32_t i = 0; i < streaming->resultCount; ++i)
        {
            ChunkMeshResult* result = &streaming->results[(streaming->resultHead + i) & queueMask];
            if (result->quads != NULL) PlatformFree(result->quads);
        }
    }

    if (streaming->entries != NULL)
    {
        for (uint32_t i = 0; i < streaming->capacity; ++i)
        {
            if (streaming->entries[i].mesh != NULL)
            {
                RendererDestroyMesh(streaming->renderer, streaming->entries[i].mesh);
            }
        }
        PlatformFree(streaming->entries);
    }

    if (streaming->spareEntries != NULL)
    {
        PlatformFree(streaming->spareEntries);
    }
    if (streaming->drawItems != NULL) PlatformFree(streaming->drawItems);
    if (streaming->requests != NULL) PlatformFree(streaming->requests);
    if (streaming->results != NULL) PlatformFree(streaming->results);
    // Потоки уже остановлены выше, поэтому примитивы синхронизации никто
    // не держит и их можно разрушить.
    PlatformConditionVariableDestroy(&streaming->workAvailable);
    PlatformMutexDestroy(&streaming->queueLock);
    PlatformFree(streaming);
}

void ChunkStreamingSetCenter(ChunkStreaming* streaming, int64_t chunkX, int64_t chunkY, int64_t chunkZ)
{
    if (streaming->hasCenter &&
        streaming->centerX == chunkX && streaming->centerY == chunkY && streaming->centerZ == chunkZ)
    {
        return;
    }

    // Вторая таблица переиспользуется при каждом переходе чанка:
    // никаких выделений и освобождений памяти в цикле кадра.
    memset(streaming->spareEntries, 0,
        (size_t)streaming->capacity * sizeof(ChunkEntry));

    streaming->hasCenter = true;
    streaming->centerX = chunkX;
    streaming->centerY = chunkY;
    streaming->centerZ = chunkZ;
    PlatformAtomicIncrementU32(&streaming->centerEpoch);

    // Пересборка таблицы с гистерезисом: живущие в радиусе + 1
    // переносятся, дальние освобождаются (отложенно, под fence).
    ChunkEntry* previousEntries = streaming->entries;
    streaming->entries = streaming->spareEntries;
    streaming->spareEntries = previousEntries;
    streaming->hasUnqueuedPending = false;
    ResetDrawList(streaming);

    for (uint32_t i = 0; i < streaming->capacity; ++i)
    {
        ChunkEntry* previous = &previousEntries[i];
        if (previous->state == CHUNK_ENTRY_EMPTY)
        {
            continue;
        }

        if (IsInsideRadius(streaming, previous->x, previous->y, previous->z, (int64_t)streaming->viewRadius + 1))
        {
            ChunkEntry* moved = InsertEntry(streaming, previous->x, previous->y, previous->z);
            moved->state = previous->state;
            moved->mesh = previous->mesh;
            moved->revision = previous->revision;
            moved->requestQueued = previous->requestQueued;
            if (moved->mesh != NULL)
            {
                AddMeshToDrawList(streaming, moved);
            }
            if (moved->state == CHUNK_ENTRY_PENDING && !moved->requestQueued)
            {
                streaming->hasUnqueuedPending = true;
            }
            previous->mesh = NULL;
            previous->drawSlotPlusOne = 0;
        }
        else if (previous->mesh != NULL)
        {
            RendererDestroyMesh(streaming->renderer, previous->mesh);
            previous->mesh = NULL;
            previous->drawSlotPlusOne = 0;
        }
    }

    QueueMissingChunks(streaming, chunkX, chunkY, chunkZ);
}

void ChunkStreamingInvalidateBlock(ChunkStreaming* streaming, int64_t blockX, int64_t blockY, int64_t blockZ)
{
    int64_t chunkX = ChunkCoordinateFromBlock(blockX);
    int64_t chunkY = ChunkCoordinateFromBlock(blockY);
    int64_t chunkZ = ChunkCoordinateFromBlock(blockZ);
    int64_t localX = blockX - chunkX * CHUNK_SIZE;
    int64_t localY = blockY - chunkY * CHUNK_SIZE;
    int64_t localZ = blockZ - chunkZ * CHUNK_SIZE;

    // Блок на границе чанка входит в расширенный регион соседа —
    // соседние чанки перестраиваются тоже.
    int64_t offsetsX[2] = { 0, localX == 0 ? -1 : (localX == CHUNK_SIZE - 1 ? 1 : 0) };
    int64_t offsetsY[2] = { 0, localY == 0 ? -1 : (localY == CHUNK_SIZE - 1 ? 1 : 0) };
    int64_t offsetsZ[2] = { 0, localZ == 0 ? -1 : (localZ == CHUNK_SIZE - 1 ? 1 : 0) };

    for (int32_t indexZ = 0; indexZ < 2; ++indexZ)
    {
        if (indexZ == 1 && offsetsZ[1] == 0) continue;
        for (int32_t indexY = 0; indexY < 2; ++indexY)
        {
            if (indexY == 1 && offsetsY[1] == 0) continue;
            for (int32_t indexX = 0; indexX < 2; ++indexX)
            {
                if (indexX == 1 && offsetsX[1] == 0) continue;

                ChunkEntry* entry = FindEntry(streaming,
                    chunkX + offsetsX[indexX], chunkY + offsetsY[indexY], chunkZ + offsetsZ[indexZ]);
                if (entry == NULL)
                {
                    continue;
                }

                // Старый меш НЕ удаляем здесь: он остаётся последней готовой
                // геометрией и продолжает рисоваться, пока рабочий поток строит
                // замену. Свап и освобождение — в ChunkStreamingPump, когда новый
                // меш загружен. Иначе чанк мигал бы дырой те кадр-два, что идёт
                // перестройка.
                entry->state = CHUNK_ENTRY_PENDING;
                entry->revision++;
                TryEnqueueRequest(streaming, entry);
            }
        }
    }
}

void ChunkStreamingPump(ChunkStreaming* streaming)
{
    uint32_t queueMask = streaming->queueCapacity - 1;
    uint32_t uploadBudget = MESH_UPLOADS_PER_FRAME;
    double pumpStart = PlatformMonotonicSeconds();

    for (;;)
    {
        // Заглянуть в очередь: результат с геометрией берём только
        // при оставшемся бюджете загрузок, остальные — бесплатны.
        PlatformMutexLock(&streaming->queueLock);
        if (streaming->resultCount == 0)
        {
            PlatformMutexUnlock(&streaming->queueLock);
            break;
        }

        ChunkMeshResult result = streaming->results[streaming->resultHead & queueMask];
        if (result.quadCount != CHUNK_MESH_BUILD_FAILED
            && result.quadCount > 0 && uploadBudget == 0)
        {
            PlatformMutexUnlock(&streaming->queueLock);
            break;
        }
        streaming->resultHead++;
        streaming->resultCount--;
        streaming->unfinishedWork--;
        PlatformMutexUnlock(&streaming->queueLock);

        ChunkEntry* entry = FindEntry(streaming, result.x, result.y, result.z);
        if (entry != NULL && entry->state == CHUNK_ENTRY_PENDING && entry->revision == result.revision)
        {
            // Заявка этой ревизии завершена; повторное выставление ниже нужно
            // только если построение или GPU-загрузка не удались.
            entry->requestQueued = false;
            if (result.quadCount == CHUNK_MESH_BUILD_FAILED)
            {
                // Сбой построения (нехватка памяти): повторная попытка
                // через условное сканирование ниже.
                streaming->hasUnqueuedPending = true;
            }
            else if (result.quadCount > 0)
            {
                RendererMesh* mesh = RendererCreateMesh(streaming->renderer, result.quads, result.quadCount);
                if (mesh != NULL)
                {
                    // Свап готов: старый меш освобождаем только теперь (отложенно
                    // под fence — кадр с ним ещё может быть в полёте на GPU).
                    bool hadMesh = entry->mesh != NULL;
                    if (hadMesh)
                    {
                        RendererDestroyMesh(streaming->renderer, entry->mesh);
                    }
                    entry->mesh = mesh;
                    if (!hadMesh)
                    {
                        AddMeshToDrawList(streaming, entry);
                    }
                    entry->state = CHUNK_ENTRY_READY;
                    uploadBudget--;
                    PlatformAtomicIncrementI64(&streaming->uploadedMeshes);
                }
                else
                {
                    streaming->hasUnqueuedPending = true;
                }
            }
            else
            {
                // Чанк стал пустым (все блоки убраны): снимаем старый меш.
                if (entry->mesh != NULL)
                {
                    RemoveMeshFromDrawList(streaming, entry);
                    RendererDestroyMesh(streaming->renderer, entry->mesh);
                    entry->mesh = NULL;
                }
                entry->state = CHUNK_ENTRY_READY;
            }
        }
        else
        {
            PlatformAtomicIncrementI64(&streaming->discardedBuilds);
        }

        if (result.quads != NULL)
        {
            PlatformFree(result.quads);
        }

        if (uploadBudget < MESH_UPLOADS_PER_FRAME)
        {
            double elapsedMilliseconds =
                (PlatformMonotonicSeconds() - pumpStart) * 1000.0;
            if (elapsedMilliseconds >= MESH_UPLOAD_BUDGET_MILLISECONDS) break;
        }
    }

    // Повторные заявки: полный проход нужен только после фактического
    // переполнения очереди, сбоя построения или загрузки.
    if (streaming->hasUnqueuedPending)
    {
        streaming->hasUnqueuedPending = false;
        for (uint32_t i = 0; i < streaming->capacity; ++i)
        {
            ChunkEntry* entry = &streaming->entries[i];
            if (entry->state == CHUNK_ENTRY_PENDING && !entry->requestQueued)
            {
                // После первого отказа unfinishedWork уже достиг ёмкости:
                // остальные попытки в этом кадре гарантированно не пройдут.
                if (!TryEnqueueRequest(streaming, entry))
                {
                    break;
                }
            }
        }
    }
}

void ChunkStreamingGetStats(ChunkStreaming* streaming,
    ChunkStreamingStats* outStats)
{
    if (streaming == NULL || outStats == NULL) return;

    outStats->queuedRequests = (uint64_t)PlatformAtomicLoadI64(&streaming->queuedRequests);
    outStats->completedBuilds = (uint64_t)PlatformAtomicLoadI64(&streaming->completedBuilds);
    outStats->cancelledBuilds = (uint64_t)PlatformAtomicLoadI64(&streaming->cancelledBuilds);
    outStats->discardedBuilds = (uint64_t)PlatformAtomicLoadI64(&streaming->discardedBuilds);
    outStats->uploadedMeshes = (uint64_t)PlatformAtomicLoadI64(&streaming->uploadedMeshes);
    int64_t totalMicroseconds = PlatformAtomicLoadI64(&streaming->totalBuildMicroseconds);

    PlatformMutexLock(&streaming->queueLock);
    outStats->pendingRequests = streaming->requestCount;
    outStats->pendingResults = streaming->resultCount;
    outStats->peakUnfinishedWork = streaming->peakUnfinishedWork;
    PlatformMutexUnlock(&streaming->queueLock);

    outStats->averageBuildMilliseconds =
        outStats->completedBuilds > 0
            ? (double)totalMicroseconds / (1000.0 * (double)outStats->completedBuilds)
            : 0.0;
}

static void ExpandFrustumPlanesForChunk(float planes[6][4])
{
    const float halfExtent = (float)(CHUNK_SIZE / 2);
    for (uint32_t plane = 0; plane < 6; ++plane)
    {
        float absoluteX = planes[plane][0] < 0.0f ? -planes[plane][0] : planes[plane][0];
        float absoluteY = planes[plane][1] < 0.0f ? -planes[plane][1] : planes[plane][1];
        float absoluteZ = planes[plane][2] < 0.0f ? -planes[plane][2] : planes[plane][2];
        planes[plane][3] += halfExtent * (absoluteX + absoluteY + absoluteZ);
    }
}

static bool FrustumContainsChunkCenter(const float planes[6][4], const float center[3])
{
    for (uint32_t plane = 0; plane < 6; ++plane)
    {
        if (planes[plane][0] * center[0]
            + planes[plane][1] * center[1]
            + planes[plane][2] * center[2]
            + planes[plane][3] < 0.0f)
        {
            return false;
        }
    }
    return true;
}

void ChunkStreamingDraw(ChunkStreaming* streaming, const float viewProjection[16],
    const int64_t renderOriginBlock[3])
{
    bool renderOriginChanged = !streaming->hasDrawRenderOrigin
        || streaming->drawRenderOriginBlock[0] != renderOriginBlock[0]
        || streaming->drawRenderOriginBlock[1] != renderOriginBlock[1]
        || streaming->drawRenderOriginBlock[2] != renderOriginBlock[2];

    // Distances and order are relative to the caller-selected render origin.
    // Recompute only when that origin or the mesh set changes.
    if (streaming->drawOrderDirty || renderOriginChanged)
    {
        for (uint32_t i = 0; i < streaming->drawItemCount; ++i)
        {
            DrawItem* item = &streaming->drawItems[i];
            const ChunkEntry* entry = &streaming->entries[item->entryIndex];
            float centerX = (float)(entry->x * CHUNK_SIZE - renderOriginBlock[0])
                + (float)(CHUNK_SIZE / 2);
            float centerY = (float)(entry->y * CHUNK_SIZE - renderOriginBlock[1])
                + (float)(CHUNK_SIZE / 2);
            float centerZ = (float)(entry->z * CHUNK_SIZE - renderOriginBlock[2])
                + (float)(CHUNK_SIZE / 2);
            item->distanceSquared = centerX * centerX + centerY * centerY + centerZ * centerZ;
        }

        SortDrawItemsFrontToBack(streaming->drawItems, streaming->drawItemCount);
        for (uint32_t i = 0; i < streaming->drawItemCount; ++i)
        {
            ChunkEntry* entry = &streaming->entries[streaming->drawItems[i].entryIndex];
            entry->drawSlotPlusOne = i + 1u;
        }

        streaming->drawRenderOriginBlock[0] = renderOriginBlock[0];
        streaming->drawRenderOriginBlock[1] = renderOriginBlock[1];
        streaming->drawRenderOriginBlock[2] = renderOriginBlock[2];
        streaming->hasDrawRenderOrigin = true;
        streaming->drawOrderDirty = false;
    }

    float planes[6][4];
    Matrix4ExtractFrustumPlanes(viewProjection, planes);
    ExpandFrustumPlanesForChunk(planes);

    // Frustum зависит от поворота камеры, поэтому отсечение остаётся
    // покадровым. Плотный список исключает обход пустых слотов hash-таблицы.
    for (uint32_t i = 0; i < streaming->drawItemCount; ++i)
    {
        const ChunkEntry* entry = &streaming->entries[streaming->drawItems[i].entryIndex];

        float chunkOriginRelative[3] = {
            (float)(entry->x * CHUNK_SIZE - renderOriginBlock[0]),
            (float)(entry->y * CHUNK_SIZE - renderOriginBlock[1]),
            (float)(entry->z * CHUNK_SIZE - renderOriginBlock[2]),
        };
        float center[3] = {
            chunkOriginRelative[0] + (float)(CHUNK_SIZE / 2),
            chunkOriginRelative[1] + (float)(CHUNK_SIZE / 2),
            chunkOriginRelative[2] + (float)(CHUNK_SIZE / 2),
        };
        // До C23 массив float[6][4] не приводится к const float(*)[4]
        // неявно, поэтому квалификатор добавляется явно.
        if (!FrustumContainsChunkCenter((const float (*)[4])planes, center))
        {
            continue;
        }

        RendererDrawMesh(streaming->renderer, entry->mesh, chunkOriginRelative);
    }
}
