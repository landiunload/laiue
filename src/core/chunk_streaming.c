#include "core/chunk_streaming.h"
#include "core/math.h"
#include "world/world.h"
#include "render/renderer.h"
#include "mesh/chunk_mesher.h"

#include <windows.h>
#include <string.h>

#define MAX_WORKER_THREADS 4

// Бюджет загрузок на GPU за один кадр: сглаживает волну готовых мешей
// при пересечении границы чанка (иначе — разовый фриз кадра).
#define MESH_UPLOADS_PER_FRAME 4

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
    uint32_t baseLow[3];
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
} ChunkRequest;

typedef struct ChunkMeshResult
{
    int64_t x;
    int64_t y;
    int64_t z;
    uint32_t revision;
    ChunkQuad* quads;
    uint32_t quadCount;
    bool succeeded;
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
    int64_t drawCameraBlockPosition[3];
    bool hasDrawCameraPosition;
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

    SRWLOCK queueLock;
    CONDITION_VARIABLE workAvailable;
    HANDLE workerThreads[MAX_WORKER_THREADS];
    uint32_t workerThreadCount;
    uint32_t desiredWorkerThreadCount;
    bool shutdownRequested;
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
    WorldGetAbsoluteBlockLow32(streaming->world,
        x * CHUNK_SIZE, y * CHUNK_SIZE, z * CHUNK_SIZE,
        entry->baseLow);
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

    AcquireSRWLockExclusive(&streaming->queueLock);
    if (streaming->unfinishedWork < streaming->queueCapacity)
    {
        uint32_t queueMask = streaming->queueCapacity - 1;
        ChunkRequest* request = &streaming->requests[(streaming->requestHead + streaming->requestCount) & queueMask];
        request->x = entry->x;
        request->y = entry->y;
        request->z = entry->z;
        request->revision = entry->revision;
        streaming->requestCount++;
        streaming->unfinishedWork++;
        enqueued = true;
    }
    ReleaseSRWLockExclusive(&streaming->queueLock);

    if (enqueued)
    {
        WakeConditionVariable(&streaming->workAvailable);
    }

    entry->requestQueued = enqueued;
    if (!enqueued)
    {
        streaming->hasUnqueuedPending = true;
    }
    return enqueued;
}

static DWORD WINAPI WorkerThreadProcedure(LPVOID parameter)
{
    ChunkStreaming* streaming = parameter;

    ChunkMesherScratch* scratch = ChunkMesherScratchCreate();
    if (scratch == NULL)
    {
        return 1;
    }

    for (;;)
    {
        AcquireSRWLockExclusive(&streaming->queueLock);
        while (!streaming->shutdownRequested && streaming->requestCount == 0)
        {
            SleepConditionVariableSRW(&streaming->workAvailable, &streaming->queueLock, INFINITE, 0);
        }
        if (streaming->shutdownRequested)
        {
            ReleaseSRWLockExclusive(&streaming->queueLock);
            ChunkMesherScratchDestroy(scratch);
            return 0;
        }

        uint32_t queueMask = streaming->queueCapacity - 1;
        ChunkRequest request = streaming->requests[streaming->requestHead & queueMask];
        streaming->requestHead++;
        streaming->requestCount--;
        ReleaseSRWLockExclusive(&streaming->queueLock);

        // Тяжёлая работа — без замка.
        ChunkMeshResult result = { .x = request.x, .y = request.y, .z = request.z, .revision = request.revision };
        result.succeeded = BuildChunkMesh(streaming->world, scratch,
            request.x, request.y, request.z, &result.quads, &result.quadCount);

        // Очередь результатов переполниться не может: unfinishedWork
        // ограничен её ёмкостью.
        AcquireSRWLockExclusive(&streaming->queueLock);
        streaming->results[(streaming->resultHead + streaming->resultCount) & queueMask] = result;
        streaming->resultCount++;
        ReleaseSRWLockExclusive(&streaming->queueLock);
    }
}

static bool StartWorkerThreads(ChunkStreaming* streaming)
{
    if (streaming->workerThreadCount != 0) return true;
    streaming->shutdownRequested = false;

    for (uint32_t index = 0;
         index < streaming->desiredWorkerThreadCount; ++index)
    {
        HANDLE thread = CreateThread(
            NULL, 0, WorkerThreadProcedure, streaming, 0, NULL);
        if (thread != NULL)
        {
            streaming->workerThreads[streaming->workerThreadCount++] = thread;
        }
    }
    return streaming->workerThreadCount != 0;
}

static bool StopWorkerThreads(ChunkStreaming* streaming)
{
    if (streaming->workerThreadCount == 0) return true;

    AcquireSRWLockExclusive(&streaming->queueLock);
    streaming->shutdownRequested = true;
    WakeAllConditionVariable(&streaming->workAvailable);
    ReleaseSRWLockExclusive(&streaming->queueLock);

    bool succeeded = true;
    for (uint32_t index = 0;
         index < streaming->workerThreadCount; ++index)
    {
        if (WaitForSingleObject(
                streaming->workerThreads[index], INFINITE) != WAIT_OBJECT_0)
        {
            succeeded = false;
        }
        CloseHandle(streaming->workerThreads[index]);
        streaming->workerThreads[index] = NULL;
    }
    streaming->workerThreadCount = 0;
    streaming->shutdownRequested = false;
    return succeeded;
}

bool ChunkStreamingPause(ChunkStreaming* streaming)
{
    if (!StopWorkerThreads(streaming)) return false;

    uint32_t queueMask = streaming->queueCapacity - 1;
    for (uint32_t index = 0; index < streaming->resultCount; ++index)
    {
        ChunkMeshResult* result = &streaming->results[
            (streaming->resultHead + index) & queueMask];
        if (result->quads != NULL)
        {
            HeapFree(GetProcessHeap(), 0, result->quads);
            result->quads = NULL;
        }
    }

    streaming->requestHead = 0;
    streaming->requestCount = 0;
    streaming->resultHead = 0;
    streaming->resultCount = 0;
    streaming->unfinishedWork = 0;
    streaming->hasUnqueuedPending = false;

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

    if (!StartWorkerThreads(streaming)) return false;
    QueueMissingChunks(streaming, newCenterX, newCenterY, newCenterZ);
    return true;
}

ChunkStreaming* ChunkStreamingCreate(World* world, Renderer* renderer, int32_t viewRadiusChunks)
{
    ChunkStreaming* streaming = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*streaming));
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

    streaming->capacity = capacity;
    streaming->queueCapacity = capacity;
    streaming->entries = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (size_t)capacity * sizeof(ChunkEntry));
    streaming->spareEntries = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
        (size_t)capacity * sizeof(ChunkEntry));
    streaming->drawItems = HeapAlloc(GetProcessHeap(), 0, (size_t)capacity * sizeof(DrawItem));
    streaming->requests = HeapAlloc(GetProcessHeap(), 0, (size_t)capacity * sizeof(ChunkRequest));
    streaming->results = HeapAlloc(GetProcessHeap(), 0, (size_t)capacity * sizeof(ChunkMeshResult));

    InitializeSRWLock(&streaming->queueLock);
    InitializeConditionVariable(&streaming->workAvailable);

    if (streaming->entries == NULL || streaming->spareEntries == NULL
        || streaming->drawItems == NULL
        || streaming->requests == NULL || streaming->results == NULL)
    {
        ChunkStreamingDestroy(streaming);
        return NULL;
    }

    // Пул потоков мешинга: масштабируется по ядрам процессора.
    SYSTEM_INFO systemInformation;
    GetSystemInfo(&systemInformation);
    streaming->desiredWorkerThreadCount =
        systemInformation.dwNumberOfProcessors > 2
        ? systemInformation.dwNumberOfProcessors - 2
        : 1;
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

    ChunkStreamingPause(streaming);

    // Остаточные результаты: освободить CPU-массивы.
    if (streaming->results != NULL)
    {
        uint32_t queueMask = streaming->queueCapacity - 1;
        for (uint32_t i = 0; i < streaming->resultCount; ++i)
        {
            ChunkMeshResult* result = &streaming->results[(streaming->resultHead + i) & queueMask];
            if (result->quads != NULL) HeapFree(GetProcessHeap(), 0, result->quads);
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
        HeapFree(GetProcessHeap(), 0, streaming->entries);
    }

    if (streaming->spareEntries != NULL)
    {
        HeapFree(GetProcessHeap(), 0, streaming->spareEntries);
    }
    if (streaming->drawItems != NULL) HeapFree(GetProcessHeap(), 0, streaming->drawItems);
    if (streaming->requests != NULL) HeapFree(GetProcessHeap(), 0, streaming->requests);
    if (streaming->results != NULL) HeapFree(GetProcessHeap(), 0, streaming->results);
    HeapFree(GetProcessHeap(), 0, streaming);
}

void ChunkStreamingSetCenter(ChunkStreaming* streaming, int64_t chunkX, int64_t chunkY, int64_t chunkZ)
{
    if (streaming->hasCenter &&
        streaming->centerX == chunkX && streaming->centerY == chunkY && streaming->centerZ == chunkZ)
    {
        return;
    }

    // Вторая таблица переиспользуется при каждом переходе чанка:
    // никаких HeapAlloc/HeapFree в игровом цикле.
    memset(streaming->spareEntries, 0,
        (size_t)streaming->capacity * sizeof(ChunkEntry));

    streaming->hasCenter = true;
    streaming->centerX = chunkX;
    streaming->centerY = chunkY;
    streaming->centerZ = chunkZ;

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
            moved->baseLow[0] = previous->baseLow[0];
            moved->baseLow[1] = previous->baseLow[1];
            moved->baseLow[2] = previous->baseLow[2];
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
    int64_t chunkX = blockX >> CHUNK_SIZE_LOG2;
    int64_t chunkY = blockY >> CHUNK_SIZE_LOG2;
    int64_t chunkZ = blockZ >> CHUNK_SIZE_LOG2;
    int64_t localX = blockX & (CHUNK_SIZE - 1);
    int64_t localY = blockY & (CHUNK_SIZE - 1);
    int64_t localZ = blockZ & (CHUNK_SIZE - 1);

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

    for (;;)
    {
        // Заглянуть в очередь: результат с геометрией берём только
        // при оставшемся бюджете загрузок, остальные — бесплатны.
        AcquireSRWLockExclusive(&streaming->queueLock);
        if (streaming->resultCount == 0)
        {
            ReleaseSRWLockExclusive(&streaming->queueLock);
            break;
        }

        ChunkMeshResult result = streaming->results[streaming->resultHead & queueMask];
        if (result.quadCount > 0 && uploadBudget == 0)
        {
            ReleaseSRWLockExclusive(&streaming->queueLock);
            break;
        }
        streaming->resultHead++;
        streaming->resultCount--;
        streaming->unfinishedWork--;
        ReleaseSRWLockExclusive(&streaming->queueLock);

        ChunkEntry* entry = FindEntry(streaming, result.x, result.y, result.z);
        if (entry != NULL && entry->state == CHUNK_ENTRY_PENDING && entry->revision == result.revision)
        {
            // Заявка этой ревизии завершена; повторное выставление ниже нужно
            // только если построение или GPU-загрузка не удались.
            entry->requestQueued = false;
            if (!result.succeeded)
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

        if (result.quads != NULL)
        {
            HeapFree(GetProcessHeap(), 0, result.quads);
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

void ChunkStreamingDraw(ChunkStreaming* streaming, const float viewProjection[16],
    const int64_t cameraBlockPosition[3])
{
    bool cameraBlockChanged = !streaming->hasDrawCameraPosition
        || streaming->drawCameraBlockPosition[0] != cameraBlockPosition[0]
        || streaming->drawCameraBlockPosition[1] != cameraBlockPosition[1]
        || streaming->drawCameraBlockPosition[2] != cameraBlockPosition[2];

    // Расстояния и порядок не зависят от поворота камеры. Пересчитываем их
    // только при переходе камеры в другой блок или изменении состава мешей.
    if (streaming->drawOrderDirty || cameraBlockChanged)
    {
        for (uint32_t i = 0; i < streaming->drawItemCount; ++i)
        {
            DrawItem* item = &streaming->drawItems[i];
            const ChunkEntry* entry = &streaming->entries[item->entryIndex];
            float centerX = (float)(entry->x * CHUNK_SIZE - cameraBlockPosition[0])
                + (float)(CHUNK_SIZE / 2);
            float centerY = (float)(entry->y * CHUNK_SIZE - cameraBlockPosition[1])
                + (float)(CHUNK_SIZE / 2);
            float centerZ = (float)(entry->z * CHUNK_SIZE - cameraBlockPosition[2])
                + (float)(CHUNK_SIZE / 2);
            item->distanceSquared = centerX * centerX + centerY * centerY + centerZ * centerZ;
        }

        SortDrawItemsFrontToBack(streaming->drawItems, streaming->drawItemCount);
        for (uint32_t i = 0; i < streaming->drawItemCount; ++i)
        {
            ChunkEntry* entry = &streaming->entries[streaming->drawItems[i].entryIndex];
            entry->drawSlotPlusOne = i + 1u;
        }

        streaming->drawCameraBlockPosition[0] = cameraBlockPosition[0];
        streaming->drawCameraBlockPosition[1] = cameraBlockPosition[1];
        streaming->drawCameraBlockPosition[2] = cameraBlockPosition[2];
        streaming->hasDrawCameraPosition = true;
        streaming->drawOrderDirty = false;
    }

    float planes[6][4];
    Matrix4ExtractFrustumPlanes(viewProjection, planes);

    // Frustum зависит от поворота камеры, поэтому отсечение остаётся
    // покадровым. Плотный список исключает обход пустых слотов hash-таблицы.
    for (uint32_t i = 0; i < streaming->drawItemCount; ++i)
    {
        const ChunkEntry* entry = &streaming->entries[streaming->drawItems[i].entryIndex];

        float chunkOriginRelative[3] = {
            (float)(entry->x * CHUNK_SIZE - cameraBlockPosition[0]),
            (float)(entry->y * CHUNK_SIZE - cameraBlockPosition[1]),
            (float)(entry->z * CHUNK_SIZE - cameraBlockPosition[2]),
        };
        float maximum[3] = {
            chunkOriginRelative[0] + (float)CHUNK_SIZE,
            chunkOriginRelative[1] + (float)CHUNK_SIZE,
            chunkOriginRelative[2] + (float)CHUNK_SIZE,
        };
        if (!FrustumIntersectsBox(planes, chunkOriginRelative, maximum))
        {
            continue;
        }

        RendererDrawMesh(streaming->renderer, entry->mesh,
            chunkOriginRelative, entry->baseLow);
    }
}
