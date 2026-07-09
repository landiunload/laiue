#include "core/chunk_streaming.h"
#include "core/math.h"
#include "world/world.h"
#include "render/renderer.h"
#include "mesh/chunk_mesher.h"

#include <windows.h>

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
    ChunkEntryState state;
} ChunkEntry;

typedef struct ChunkRequest
{
    int64_t x;
    int64_t y;
    int64_t z;
} ChunkRequest;

typedef struct ChunkMeshResult
{
    int64_t x;
    int64_t y;
    int64_t z;
    ChunkVertex* vertices;
    uint32_t vertexCount;
    uint32_t* indices;
    uint32_t indexCount;
} ChunkMeshResult;

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

    // Кольцевые очереди под общим замком; ёмкость гарантирует,
    // что число заявок в полёте никогда не превысит очередь результатов.
    ChunkRequest* requests;
    uint32_t requestHead;
    uint32_t requestCount;
    ChunkMeshResult* results;
    uint32_t resultHead;
    uint32_t resultCount;
    uint32_t queueCapacity;

    SRWLOCK queueLock;
    CONDITION_VARIABLE workAvailable;
    HANDLE workerThread;
    bool shutdownRequested;
};

static uint32_t HashChunkCoordinate(int64_t x, int64_t y, int64_t z)
{
    uint64_t hash = (uint64_t)x * 73856093ULL
                  ^ (uint64_t)y * 19349663ULL
                  ^ (uint64_t)z * 83492791ULL;
    return (uint32_t)(hash ^ (hash >> 33));
}

static ChunkEntry* FindEntry(const ChunkStreaming* streaming, int64_t x, int64_t y, int64_t z)
{
    uint32_t mask = streaming->capacity - 1;
    uint32_t index = HashChunkCoordinate(x, y, z) & mask;

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
    uint32_t index = HashChunkCoordinate(x, y, z) & mask;

    while (streaming->entries[index].state != CHUNK_ENTRY_EMPTY)
    {
        index = (index + 1) & mask;
    }

    ChunkEntry* entry = &streaming->entries[index];
    entry->x = x;
    entry->y = y;
    entry->z = z;
    entry->mesh = NULL;
    return entry;
}

static bool IsInsideRadius(const ChunkStreaming* streaming, int64_t x, int64_t y, int64_t z)
{
    int64_t deltaX = x - streaming->centerX;
    int64_t deltaY = y - streaming->centerY;
    int64_t deltaZ = z - streaming->centerZ;
    if (deltaX < 0) deltaX = -deltaX;
    if (deltaY < 0) deltaY = -deltaY;
    if (deltaZ < 0) deltaZ = -deltaZ;
    int64_t radius = streaming->viewRadius;
    return deltaX <= radius && deltaY <= radius && deltaZ <= radius;
}

static DWORD WINAPI WorkerThreadProcedure(LPVOID parameter)
{
    ChunkStreaming* streaming = parameter;

    for (;;)
    {
        AcquireSRWLockExclusive(&streaming->queueLock);
        while (!streaming->shutdownRequested && streaming->requestCount == 0)
        {
            SleepConditionVariableSRW(&streaming->workAvailable, &streaming->queueLock, INFINITE, 0);
        }
        if (streaming->shutdownRequested && streaming->requestCount == 0)
        {
            ReleaseSRWLockExclusive(&streaming->queueLock);
            return 0;
        }

        uint32_t queueMask = streaming->queueCapacity - 1;
        ChunkRequest request = streaming->requests[streaming->requestHead & queueMask];
        streaming->requestHead++;
        streaming->requestCount--;
        ReleaseSRWLockExclusive(&streaming->queueLock);

        // Тяжёлая работа — без замка.
        ChunkMeshResult result = { .x = request.x, .y = request.y, .z = request.z };
        BuildChunkMesh(streaming->world, request.x, request.y, request.z,
            &result.vertices, &result.vertexCount, &result.indices, &result.indexCount);

        AcquireSRWLockExclusive(&streaming->queueLock);
        if (streaming->resultCount < streaming->queueCapacity)
        {
            streaming->results[(streaming->resultHead + streaming->resultCount) & queueMask] = result;
            streaming->resultCount++;
        }
        else
        {
            // Не должно случаться: ёмкости очередей совпадают.
            if (result.vertices != NULL) HeapFree(GetProcessHeap(), 0, result.vertices);
            if (result.indices != NULL) HeapFree(GetProcessHeap(), 0, result.indices);
        }
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

    // Ёмкости — степени двойки с двукратным запасом от объёма радиуса.
    uint32_t diameter = (uint32_t)(viewRadiusChunks * 2 + 1);
    uint32_t volume = diameter * diameter * diameter;
    uint32_t capacity = 1;
    while (capacity < volume * 2)
    {
        capacity <<= 1;
    }

    streaming->capacity = capacity;
    streaming->queueCapacity = capacity;
    streaming->entries = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (size_t)capacity * sizeof(ChunkEntry));
    streaming->requests = HeapAlloc(GetProcessHeap(), 0, (size_t)capacity * sizeof(ChunkRequest));
    streaming->results = HeapAlloc(GetProcessHeap(), 0, (size_t)capacity * sizeof(ChunkMeshResult));

    InitializeSRWLock(&streaming->queueLock);
    InitializeConditionVariable(&streaming->workAvailable);

    if (streaming->entries == NULL || streaming->requests == NULL || streaming->results == NULL)
    {
        ChunkStreamingDestroy(streaming);
        return NULL;
    }

    streaming->workerThread = CreateThread(NULL, 0, WorkerThreadProcedure, streaming, 0, NULL);
    if (streaming->workerThread == NULL)
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

    if (streaming->workerThread != NULL)
    {
        AcquireSRWLockExclusive(&streaming->queueLock);
        streaming->shutdownRequested = true;
        // Невыполненные заявки не нужны — поток завершится быстрее.
        streaming->requestCount = 0;
        WakeAllConditionVariable(&streaming->workAvailable);
        ReleaseSRWLockExclusive(&streaming->queueLock);

        WaitForSingleObject(streaming->workerThread, INFINITE);
        CloseHandle(streaming->workerThread);
    }

    // Остаточные результаты: освободить CPU-массивы.
    if (streaming->results != NULL)
    {
        uint32_t queueMask = streaming->queueCapacity - 1;
        for (uint32_t i = 0; i < streaming->resultCount; ++i)
        {
            ChunkMeshResult* result = &streaming->results[(streaming->resultHead + i) & queueMask];
            if (result->vertices != NULL) HeapFree(GetProcessHeap(), 0, result->vertices);
            if (result->indices != NULL) HeapFree(GetProcessHeap(), 0, result->indices);
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

    streaming->hasCenter = true;
    streaming->centerX = chunkX;
    streaming->centerY = chunkY;
    streaming->centerZ = chunkZ;

    // Пересборка таблицы: живущие в радиусе переносятся,
    // вышедшие из радиуса — освобождаются (отложенно, под fence).
    ChunkEntry* previousEntries = streaming->entries;
    ChunkEntry* freshEntries = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
        (size_t)streaming->capacity * sizeof(ChunkEntry));
    if (freshEntries == NULL)
    {
        return;
    }

    streaming->entries = freshEntries;
    for (uint32_t i = 0; i < streaming->capacity; ++i)
    {
        ChunkEntry* previous = &previousEntries[i];
        if (previous->state == CHUNK_ENTRY_EMPTY)
        {
            continue;
        }

        if (IsInsideRadius(streaming, previous->x, previous->y, previous->z))
        {
            ChunkEntry* moved = InsertEntry(streaming, previous->x, previous->y, previous->z);
            moved->state = previous->state;
            moved->mesh = previous->mesh;
        }
        else if (previous->mesh != NULL)
        {
            RendererDestroyMesh(streaming->renderer, previous->mesh);
        }
    }
    HeapFree(GetProcessHeap(), 0, previousEntries);

    // Заказ недостающих чанков оболочками — от ближних к дальним.
    AcquireSRWLockExclusive(&streaming->queueLock);
    uint32_t queueMask = streaming->queueCapacity - 1;
    bool enqueuedAny = false;

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
                    if (streaming->requestCount >= streaming->queueCapacity)
                    {
                        continue;
                    }

                    ChunkEntry* entry = InsertEntry(streaming, x, y, z);
                    entry->state = CHUNK_ENTRY_PENDING;

                    ChunkRequest* request = &streaming->requests[(streaming->requestHead + streaming->requestCount) & queueMask];
                    request->x = x;
                    request->y = y;
                    request->z = z;
                    streaming->requestCount++;
                    enqueuedAny = true;
                }
            }
        }
    }

    ReleaseSRWLockExclusive(&streaming->queueLock);
    if (enqueuedAny)
    {
        WakeAllConditionVariable(&streaming->workAvailable);
    }
}

void ChunkStreamingPump(ChunkStreaming* streaming)
{
    uint32_t queueMask = streaming->queueCapacity - 1;

    for (;;)
    {
        AcquireSRWLockExclusive(&streaming->queueLock);
        if (streaming->resultCount == 0)
        {
            ReleaseSRWLockExclusive(&streaming->queueLock);
            return;
        }
        ChunkMeshResult result = streaming->results[streaming->resultHead & queueMask];
        streaming->resultHead++;
        streaming->resultCount--;
        ReleaseSRWLockExclusive(&streaming->queueLock);

        ChunkEntry* entry = FindEntry(streaming, result.x, result.y, result.z);
        if (entry != NULL && entry->state == CHUNK_ENTRY_PENDING &&
            IsInsideRadius(streaming, result.x, result.y, result.z))
        {
            if (result.vertexCount > 0)
            {
                entry->mesh = RendererCreateMesh(streaming->renderer,
                    result.vertices, result.vertexCount, result.indices, result.indexCount);
            }
            entry->state = CHUNK_ENTRY_READY;
        }

        if (result.vertices != NULL) HeapFree(GetProcessHeap(), 0, result.vertices);
        if (result.indices != NULL) HeapFree(GetProcessHeap(), 0, result.indices);
    }
}

void ChunkStreamingDraw(const ChunkStreaming* streaming, const float viewProjection[16])
{
    float planes[6][4];
    Matrix4ExtractFrustumPlanes(viewProjection, planes);

    for (uint32_t i = 0; i < streaming->capacity; ++i)
    {
        const ChunkEntry* entry = &streaming->entries[i];
        if (entry->state != CHUNK_ENTRY_READY || entry->mesh == NULL)
        {
            continue;
        }

        float minimum[3] = {
            (float)(entry->x * CHUNK_SIZE),
            (float)(entry->y * CHUNK_SIZE),
            (float)(entry->z * CHUNK_SIZE),
        };
        float maximum[3] = {
            minimum[0] + (float)CHUNK_SIZE,
            minimum[1] + (float)CHUNK_SIZE,
            minimum[2] + (float)CHUNK_SIZE,
        };

        if (FrustumIntersectsBox(planes, minimum, maximum))
        {
            RendererDrawMesh(streaming->renderer, entry->mesh, minimum);
        }
    }
}
