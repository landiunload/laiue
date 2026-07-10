#include "core/chunk_streaming.h"
#include "core/math.h"
#include "world/world.h"
#include "render/renderer.h"
#include "mesh/chunk_mesher.h"

#include <windows.h>

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

typedef struct ChunkEntry
{
    int64_t x;
    int64_t y;
    int64_t z;
    RendererMesh* mesh;
    uint32_t revision;      // растёт при инвалидации: устаревшие результаты отбрасываются
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
    uint32_t capacity;

    DrawItem* drawItems;    // переиспользуемый буфер сортировки отрисовки

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
    bool shutdownRequested;
};

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

    // Ёмкости — степени двойки с двукратным запасом от объёма радиуса
    // (гистерезис держит записи до радиуса + 1).
    uint32_t diameter = (uint32_t)(viewRadiusChunks * 2 + 3);
    uint32_t volume = diameter * diameter * diameter;
    uint32_t capacity = 1;
    while (capacity < volume * 2)
    {
        capacity <<= 1;
    }

    streaming->capacity = capacity;
    streaming->queueCapacity = capacity;
    streaming->entries = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (size_t)capacity * sizeof(ChunkEntry));
    streaming->drawItems = HeapAlloc(GetProcessHeap(), 0, (size_t)capacity * sizeof(DrawItem));
    streaming->requests = HeapAlloc(GetProcessHeap(), 0, (size_t)capacity * sizeof(ChunkRequest));
    streaming->results = HeapAlloc(GetProcessHeap(), 0, (size_t)capacity * sizeof(ChunkMeshResult));

    InitializeSRWLock(&streaming->queueLock);
    InitializeConditionVariable(&streaming->workAvailable);

    if (streaming->entries == NULL || streaming->drawItems == NULL ||
        streaming->requests == NULL || streaming->results == NULL)
    {
        ChunkStreamingDestroy(streaming);
        return NULL;
    }

    // Пул потоков мешинга: масштабируется по ядрам процессора.
    SYSTEM_INFO systemInformation;
    GetSystemInfo(&systemInformation);
    uint32_t workerCount = systemInformation.dwNumberOfProcessors > 2
        ? systemInformation.dwNumberOfProcessors - 2
        : 1;
    if (workerCount > MAX_WORKER_THREADS)
    {
        workerCount = MAX_WORKER_THREADS;
    }

    for (uint32_t i = 0; i < workerCount; ++i)
    {
        HANDLE thread = CreateThread(NULL, 0, WorkerThreadProcedure, streaming, 0, NULL);
        if (thread != NULL)
        {
            streaming->workerThreads[streaming->workerThreadCount++] = thread;
        }
    }

    if (streaming->workerThreadCount == 0)
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

    if (streaming->workerThreadCount > 0)
    {
        AcquireSRWLockExclusive(&streaming->queueLock);
        streaming->shutdownRequested = true;
        WakeAllConditionVariable(&streaming->workAvailable);
        ReleaseSRWLockExclusive(&streaming->queueLock);

        WaitForMultipleObjects(streaming->workerThreadCount, streaming->workerThreads, TRUE, INFINITE);
        for (uint32_t i = 0; i < streaming->workerThreadCount; ++i)
        {
            CloseHandle(streaming->workerThreads[i]);
        }
    }

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

    // Свежая таблица выделяется ДО смены центра: при нехватке памяти
    // состояние не трогается, попытка повторится в следующем кадре.
    ChunkEntry* freshEntries = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
        (size_t)streaming->capacity * sizeof(ChunkEntry));
    if (freshEntries == NULL)
    {
        return;
    }

    streaming->hasCenter = true;
    streaming->centerX = chunkX;
    streaming->centerY = chunkY;
    streaming->centerZ = chunkZ;

    // Пересборка таблицы с гистерезисом: живущие в радиусе + 1
    // переносятся, дальние освобождаются (отложенно, под fence).
    ChunkEntry* previousEntries = streaming->entries;
    streaming->entries = freshEntries;

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
        }
        else if (previous->mesh != NULL)
        {
            RendererDestroyMesh(streaming->renderer, previous->mesh);
        }
    }
    HeapFree(GetProcessHeap(), 0, previousEntries);

    // Заказ недостающих чанков оболочками — от ближних к дальним.
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
                    if (chebyshev != shell)
                    {
                        continue;
                    }

                    int64_t x = chunkX + deltaX;
                    int64_t y = chunkY + deltaY;
                    int64_t z = chunkZ + deltaZ;
                    if (FindEntry(streaming, x, y, z) != NULL)
                    {
                        continue;
                    }

                    ChunkEntry* entry = InsertEntry(streaming, x, y, z);
                    entry->state = CHUNK_ENTRY_PENDING;
                    TryEnqueueRequest(streaming, entry);
                }
            }
        }
    }
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

                if (entry->mesh != NULL)
                {
                    RendererDestroyMesh(streaming->renderer, entry->mesh);
                    entry->mesh = NULL;
                }
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
            if (!result.succeeded)
            {
                // Сбой построения (нехватка памяти): повторная попытка
                // через сканирование ниже.
                entry->requestQueued = false;
            }
            else if (result.quadCount > 0)
            {
                RendererMesh* mesh = RendererCreateMesh(streaming->renderer, result.quads, result.quadCount);
                if (mesh != NULL)
                {
                    entry->mesh = mesh;
                    entry->state = CHUNK_ENTRY_READY;
                    uploadBudget--;
                }
                else
                {
                    entry->requestQueued = false;
                }
            }
            else
            {
                entry->state = CHUNK_ENTRY_READY;
            }
        }

        if (result.quads != NULL)
        {
            HeapFree(GetProcessHeap(), 0, result.quads);
        }
    }

    // Повторные заявки: записи, оставшиеся без заявки (переполнение
    // очереди, сбой построения или загрузки).
    for (uint32_t i = 0; i < streaming->capacity; ++i)
    {
        ChunkEntry* entry = &streaming->entries[i];
        if (entry->state == CHUNK_ENTRY_PENDING && !entry->requestQueued)
        {
            TryEnqueueRequest(streaming, entry);
        }
    }
}

void ChunkStreamingDraw(ChunkStreaming* streaming, const float viewProjection[16],
    const int64_t cameraBlockPosition[3])
{
    float planes[6][4];
    Matrix4ExtractFrustumPlanes(viewProjection, planes);

    // Сбор видимых мешей с расстоянием до камеры.
    uint32_t drawCount = 0;
    for (uint32_t i = 0; i < streaming->capacity; ++i)
    {
        const ChunkEntry* entry = &streaming->entries[i];
        if (entry->state != CHUNK_ENTRY_READY || entry->mesh == NULL)
        {
            continue;
        }

        float minimum[3] = {
            (float)(entry->x * CHUNK_SIZE - cameraBlockPosition[0]),
            (float)(entry->y * CHUNK_SIZE - cameraBlockPosition[1]),
            (float)(entry->z * CHUNK_SIZE - cameraBlockPosition[2]),
        };
        float maximum[3] = {
            minimum[0] + (float)CHUNK_SIZE,
            minimum[1] + (float)CHUNK_SIZE,
            minimum[2] + (float)CHUNK_SIZE,
        };

        if (!FrustumIntersectsBox(planes, minimum, maximum))
        {
            continue;
        }

        float centerX = minimum[0] + (float)(CHUNK_SIZE / 2);
        float centerY = minimum[1] + (float)(CHUNK_SIZE / 2);
        float centerZ = minimum[2] + (float)(CHUNK_SIZE / 2);

        streaming->drawItems[drawCount].distanceSquared = centerX * centerX + centerY * centerY + centerZ * centerZ;
        streaming->drawItems[drawCount].entryIndex = i;
        drawCount++;
    }

    // Сортировка спереди-назад: ближние чанки заполняют глубину первыми,
    // перекрытые пиксели дальних отсекаются early-Z.
    for (uint32_t i = 1; i < drawCount; ++i)
    {
        DrawItem item = streaming->drawItems[i];
        uint32_t position = i;
        while (position > 0 && streaming->drawItems[position - 1].distanceSquared > item.distanceSquared)
        {
            streaming->drawItems[position] = streaming->drawItems[position - 1];
            position--;
        }
        streaming->drawItems[position] = item;
    }

    for (uint32_t i = 0; i < drawCount; ++i)
    {
        const ChunkEntry* entry = &streaming->entries[streaming->drawItems[i].entryIndex];

        float chunkOriginRelative[3] = {
            (float)(entry->x * CHUNK_SIZE - cameraBlockPosition[0]),
            (float)(entry->y * CHUNK_SIZE - cameraBlockPosition[1]),
            (float)(entry->z * CHUNK_SIZE - cameraBlockPosition[2]),
        };
        uint32_t chunkBaseLow[3] = {
            (uint32_t)(uint64_t)(entry->x * CHUNK_SIZE),
            (uint32_t)(uint64_t)(entry->y * CHUNK_SIZE),
            (uint32_t)(uint64_t)(entry->z * CHUNK_SIZE),
        };

        RendererDrawMesh(streaming->renderer, entry->mesh, chunkOriginRelative, chunkBaseLow);
    }
}
